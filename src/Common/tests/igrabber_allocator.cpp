#if defined(NDEBUG)
#undef NDEBUG
#endif

#include <cassert>

#include <thread>


#include <atomic>
#include <cstddef>
#include <mutex>
#include <memory>
#include <random>
#include <type_traits>
#include <unordered_map>
#include <list>

#include <boost/intrusive/list.hpp>
#include <boost/intrusive/set.hpp>
#include <boost/noncopyable.hpp>

#include <common/logger_useful.h>

#include <Common/Allocators/IGrabberAllocator_fwd.h>
#include <Common/Allocators/allocatorCommon.h>


namespace DB
{

namespace ErrorCodes
{
extern const int BAD_ARGUMENTS;
extern const int CANNOT_ALLOCATE_MEMORY;
extern const int CANNOT_MUNMAP;
}

/**
 * @brief This class represents a tool combining a reference-counted memory block cache integrated with a mmap-backed
 *        memory allocator. Overall memory limit, begin set by the user, is constant, so this tool also performs
 *        cache entries eviction if needed.
 *
 * @warning This allocator is meant to be used with dynamic data structures as #TValue (those of small sizeof referring
 *          to memory allocated somewhere in the heap, e.g. std::vector).
 *          DO NOT USE IT WITH STACK ALLOCATED STRUCTURES, THE PROGRAM WILL NOT WORK AS INTENDED.
 *          See IGrabberAllocator::RegionMetadata for detail.
 *
 * @section motivation Motivation
 *
 *   - Domain-specific cache has a memory usage limit (and we want it to include allocator fragmentation overhead).
 *   - Memory fragmentation can be bridged over using cache eviction.
 *   - We do not waste space for reclaimed memory regions. Ordinarily, they'd get into default allocator raw cache
 *     after being freed.
 *   - Direct memory allocation serves for debugging purposes: placing mmap'd memory far from malloc'd facilitates
 *     revealing memory stomping bugs.
 *
 * @section overview Overview
 * @subsection size_func #SizeFunction
 *
 * If specified, interface is thought to have a @e run-time function which needs to access the object's state
 * to measure its size.
 *
 * Target's @c operator() is thought to have a signature (void) -> @c T, @c T convertible to size_t.
 *
 * For example, consider the cache for some @c CacheItem : PODArray. The PODArray itself is just a few pointers and
 * size_t's, but the size we want can be obtained only by examining the @c CacheItem object.
 *
 * As opposed to that, some objects (usually, of fixed size) do not need to be examined.
 * Consider some @c OtherCacheItem representing some sort of a heap-allocated std::array (of compile-time known) fixed
 * size. This knowledge greatly simplifies used algorithms (no need of coalescing and so on).
 *
 * @subsection init_func #InitFunction
 *
 * Function is supposed to produce a #Value and bind its data references (e.g. T* data in std::vector<T>)
 * to a given pointer.
 *
 * Target's @c operator() is thought to have a signature (void * start) -> #Value.
 *
 * If specified as a function template parameter, the class should not change the global state. However, that isn't
 * always possible, so IGrabberAllocator::getOrSet has an overload taking this functor. For example, on new item
 * insertion (if not found) the code could do so metrics update.
 *
 * @tparam TKey Object that will be used to @e quickly address #Value.
 * @tparam TValue Type of objects being stored (usually thought to be some child of PODArray).
 *
 * @tparam KeyHash Struct generating a compile-time size_t hash for #Key. By default, std::hash is used.
 *
 * @tparam SizeFunction Struct generating a size_t that approximates (as an upper bound) #Value's size.
 *
 * @tparam ASLRFunction Function that, given a random number generator, outputs the address in heap, which
 *         will be used as a starting point to place the object.
 *         Must meet the requirements of gnu::pure function attribute.
 *         By default, AllocatorsASLR is used.
 *
 * @note Cache is not NUMA friendly.
 */
template <
    class TKey,
    class TValue,
    class KeyHash,
    class SizeFunction,
    class InitFunction,
    class ASLRFunction,
    size_t MinChunkSize,
    size_t ValueAlignment>
class IGrabberAllocator : private boost::noncopyable
{
private:
    static_assert(std::is_copy_constructible_v<TKey>,
            "Key should be copy-constructible, see IGrabberAllocator::RegionMetadata::init_key for motivation");

    static constexpr const size_t page_size = 4096;

    struct RegionMetadata;

    mutable std::mutex mutex;

    static constexpr auto ASLR = ASLRFunction();
    static constexpr auto getSize = SizeFunction();
    static constexpr auto initValue = InitFunction();

    Logger& log;

/**
 * Constructing, destructing, and usings.
 */
public:
    using Key = TKey;
    using Value = TValue;

    /**
     * @brief Output type, tracks reference counf (@ref overview) via a custom Deleter.
     */
    using ValuePtr = std::shared_ptr<Value>;

    /**
     * @param max_cache_size_ upper bound on cache size. Must be >= #MinChunkSize (or, if it is not specified,
     *        ga::defaultMinChunkSize). If this constraint is not satisfied, a POCO Exception is thrown.
     */
    constexpr IGrabberAllocator(size_t max_cache_size_)
        : log(Logger::get("IGrabberAllocator")),
          max_cache_size(max_cache_size_)
        {
            if (max_cache_size < MinChunkSize)
                throw Exception("Cache max size must not be less than MinChunkSize",
                        ErrorCodes::BAD_ARGUMENTS);
        }

    ~IGrabberAllocator() noexcept
    {
        std::scoped_lock lock(mutex, used_regions_mutex);

        used_regions.clear();
        unused_regions.clear();
        free_regions.clear();

        all_regions.clear_and_dispose(region_metadata_disposer);
    }

/**
 * Statistics storage.
 */
private:
    const size_t max_cache_size;

    size_t total_chunks_size {0};
    size_t total_allocated_size {0};

    /// Part of #max_cache_size that is currently used.
    std::atomic_size_t total_size_in_use {0};

    std::atomic_size_t total_size_currently_initialized {0};

    /// Value was in the cache.
    std::atomic_size_t hits {0};

    /// Value we were waiting for was calculated by another thread.
    /// @note Also summed in #hits.
    std::atomic_size_t concurrent_hits {0};

    /// Value was not found in the cache.
    std::atomic_size_t misses {0};

    size_t allocations {0};
    size_t allocated_bytes {0};
    size_t evictions {0};
    size_t evicted_bytes {0};
    size_t secondary_evictions {0};

/**
 * Reset and statistics utilities
 */
public:
    /// @return Total size (in bytes) occupied by the cache (does NOT include metadata size).
    constexpr size_t getSizeInUse() const noexcept
    {
        return total_size_in_use.load();
    }

    /// @return Count of pairs (#Key, #Value) that are currently in use.
    inline size_t getUsedRegionsCount() const noexcept
    {
        std::lock_guard used_lock(used_regions_mutex);
        return used_regions.size();
    }

    /**
     * @brief Removes unused elements from the cache. Such include unused (unreferenced) RegionMetadata's, as well as
     *        allocated but unused MemoryChunks.
     *
     * @note This method does NOT invalidate used elements. They remain in the cache.
     *
     */
    void shrinkToFit(bool clear_stats = true)
    {
        std::lock_guard global_lock(mutex);

        insertion_attempts.clear();

        // value_to_region not cleared because it contains referenced regions.

        const auto disposer = [this](auto& container)
        {
            for (const auto& e : container)
                all_regions.erase(all_regions.iterator_to(e));

            container.clear_and_dispose(region_metadata_disposer);
        };

        disposer(unused_regions);
        disposer(free_regions);

        chunks.remove_if([](const auto& chunk) { return chunk.used_refcount.load() == 0; });

        if (!clear_stats)
            return;

        total_chunks_size = 0;
        total_allocated_size = 0;
        total_size_in_use.store(0);

        total_size_currently_initialized.store(0, std::memory_order_relaxed);

        hits.store(0, std::memory_order_relaxed);
        concurrent_hits.store(0, std::memory_order_relaxed);
        misses.store(0, std::memory_order_relaxed);

        allocations  = 0;
        allocated_bytes = 0;
        evictions = 0;
        evicted_bytes = 0;
        secondary_evictions = 0;
    }

    /**
     * @warning Currently this method acts like IGrabberAllocator::shrinkToFit, the proposed action is below.
     *
     *  Invalidates the cache. All unused elements (along with their metadata) are removed, all used elements
     *  become "disposed" (tracked in RegionMetadata) and are removed on first IGrabberAllocator::get or
     *  IGrabberAllocator::getOrSet call.
     */
    void reset()
    {
        shrinkToFit();
    }

    /**
     * @note Metadata is allocated by a usual allocator (malloc/new) so its memory usage is not accounted.
     */
    [[nodiscard]] ga::Stats getStats() const noexcept
    {
        ga::Stats out = {};

        {
            std::lock_guard global_lock(mutex);

            out.chunks_size = total_chunks_size;
            out.allocated_size = total_allocated_size;
            out.initialized_size = total_size_currently_initialized.load(std::memory_order_relaxed);
            out.used_size = total_size_in_use.load(std::memory_order_relaxed);

            out.chunks = chunks.size();
            out.regions = all_regions.size();
            out.free_regions = free_regions.size();
            out.unused_regions = unused_regions.size();

            out.hits = hits.load(std::memory_order_relaxed);
            out.concurrent_hits = concurrent_hits.load(std::memory_order_relaxed);
            out.misses = misses.load(std::memory_order_relaxed);

            out.allocations = allocations;
            out.allocated_bytes = allocated_bytes;
            out.evictions = evictions;
            out.evicted_bytes = evicted_bytes;
            out.secondary_evictions = secondary_evictions;
        }

        std::lock_guard used_lock(used_regions_mutex);

        out.used_regions = used_regions.size();

        return out;
    }

/**
 * Memory storage.
 */
private:
    /**
     * Each memory region can be:
     * - Free: not holding any data.
     * - Allocated, but unused (can be evicted).
     * - Allocated and used (addressed by #Key, can't be evicted).
     */

    struct UnusedRegionTag;
    struct RegionTag;
    struct FreeRegionTag;
    struct UsedRegionTag;

    using TUnusedRegionHook  = boost::intrusive::list_base_hook<boost::intrusive::tag<UnusedRegionTag>>;
    using TAllRegionsHook    = boost::intrusive::list_base_hook<boost::intrusive::tag<RegionTag>>;
    using TFreeRegionHook    = boost::intrusive::set_base_hook< boost::intrusive::tag<FreeRegionTag>>;
    using TUsedRegionHook    = boost::intrusive::set_base_hook<boost::intrusive::tag<UsedRegionTag>>;

    struct RegionCompareBySize;
    struct RegionCompareByKey;

    using TUnusedRegionsList = boost::intrusive::list<RegionMetadata,
        boost::intrusive::base_hook<TUnusedRegionHook>,
        boost::intrusive::constant_time_size<true>>;
    using TRegionsList = boost::intrusive::list<RegionMetadata,
        boost::intrusive::base_hook<TAllRegionsHook>,
        boost::intrusive::constant_time_size<true>>;
    using TFreeRegionsMap = boost::intrusive::multiset<RegionMetadata,
        boost::intrusive::compare<RegionCompareBySize>,
        boost::intrusive::base_hook<TFreeRegionHook>,
        boost::intrusive::constant_time_size<true>>;
    using TUsedRegionsMap = boost::intrusive::set<RegionMetadata,
        boost::intrusive::compare<RegionCompareByKey>,
        boost::intrusive::base_hook<TUsedRegionHook>,
        boost::intrusive::constant_time_size<true>>;

    /**
     * @brief Represented by an adjacency list (boost linked list). Used to find region's free neighbours to coalesce
     *        on eviction.
     *
     * @anchor ds_start.
     */
    TRegionsList all_regions;

    /**
     * Represented by a size multimap.
     * Used to find free region with at least requested size.
     */
    TFreeRegionsMap free_regions;

    /**
     * Represented by a keymap (boost multiset).
     */
    TUsedRegionsMap used_regions;
    mutable std::mutex used_regions_mutex;

    /**
     * Represented by a LRU List (boost linked list).
     * Used to find the next region for eviction.
     * TODO Replace with exponentially-smoothed size-weighted LFU map.
     */
    TUnusedRegionsList unused_regions;

    /**
     * Used to identify metadata associated with some #Value (needed while deleting #ValuePtr).
     * @see IGrabberAllocator::getImpl
     */
    std::unordered_map<const Value*, RegionMetadata*> value_to_region;

    struct MemoryChunk;
    std::list<MemoryChunk> chunks;

    struct InsertionAttempt;
    std::unordered_map<Key, std::shared_ptr<InsertionAttempt>, KeyHash> insertion_attempts;
    std::mutex attempts_mutex;

/**
 * Getting and setting utilities
 */
public:
    /**
     * @brief Tries to find the value for a given #key in the cache.
     *
     * @return value if found.
     * @return nullptr otherwise.
     */
    inline ValuePtr get(const Key& key)
    {
        ValuePtr res = getImpl(key);

        if (res)
            ++hits;
        else
            ++misses;

        return res;
    }

    using GetOrSetRet = std::pair<ValuePtr, bool>;

    template<class S = SizeFunction, class I = InitFunction>
    inline typename std::enable_if<!std::is_same_v<S, ga::Runtime> &&
                                   !std::is_same_v<I, ga::Runtime>,
        GetOrSetRet>::type getOrSet(const Key & key)
    {
        return getOrSet(key, getSize, initValue);
    }

    template<class Init, class S = SizeFunction, class I = InitFunction>
    inline typename std::enable_if<!std::is_same_v<S, ga::Runtime> &&
                                    std::is_same_v<I, ga::Runtime>,
        GetOrSetRet>::type getOrSet(const Key & key, Init && init_func)
    {
        return getOrSet(key, getSize, init_func);
    }

    template<class Size, class S = SizeFunction, class I = InitFunction>
    inline typename std::enable_if<std::is_same_v<S, ga::Runtime> &&
                                  !std::is_same_v<I, ga::Runtime>,
        GetOrSetRet>::type getOrSet(const Key & key, Size && size_func)
    {
        return getOrSet(key, size_func, initValue);
    }

    /**
     * @brief If the value for the key is in the cache, returns it.
     *        Otherwise, calls #get_size to obtain required size of storage for the key, then allocates storage and
     *        calls #initialize for necessary initialization before data from cache could be used.
     *
     * @note Only one of several concurrent threads calling this method will call #get_size or #initialize functors.
     *       Others will wait for that call to complete and will use its result (this helps to prevent cache stampede).
     *
     * @note Exceptions that occur in callback functors (#get_size and #initialize) will be propagated to the caller.
     *       Another thread from the set of concurrent threads will then try to call its callbacks et cetera.
     *
     * @note This function does NOT throw on invalid input (e.g. when #get_size result is 0). It simply returns a
     *       {nullptr, false}. However, there is a special case when the allocator failed to allocate the desired size
     *       (e.g. when #get_size result is greater than #max_cache_size). In this case {nullptr, true} is returned.
     *
     * @note During insertion, each key is locked - to avoid parallel initialization of regions for same key.
     *
     * @param key Key that is used to find the target value.
     *
     * @param get_size Callback functor that is called if value with a given #key was neither found in cache
     *      nor produced by any of other threads.
     *
     * @param initialize Callback function that is called if
     *      <ul>
     *      <li>#get_size is called and doesn't throw.</li>
     *      <li>Allocator did not fail to allocate another memory region.</li>
     *      </ul>
     *
     * @return Cached value or nullptr if the cache is full and used.
     * @return Bool indicating whether the value was produced during the call. <br>
     *      False on cache hit (including a concurrent cache hit), true on cache miss).
     */
    template <class Init, class Size>
    inline GetOrSetRet getOrSet(const Key & key, Size && get_size, Init && initialize)
    {
        //printf("Thread %lu getOrSet %d\n", pthread_self(), key);

        if (ValuePtr out = getImpl(key); out)
        {
            printf("Thread %lu getOrSet found\n", pthread_self());

            ++hits;
            return {out, false}; // value was found in the cache.
        }

        //printf("Thread %lu getOrSet not found\n", pthread_self());

        InsertionAttemptDisposer disposer;
        InsertionAttempt * attempt;

        {
            std::lock_guard att_lock(attempts_mutex);

            auto & insertion_attempt = insertion_attempts[key];

            if (!insertion_attempt)
                insertion_attempt = std::make_shared<InsertionAttempt>(*this);

            disposer.acquire(&key, insertion_attempt);
        }

        attempt = disposer.attempt.get();

        std::lock_guard attempt_lock(attempt->mutex);

        {
            disposer.attempt_disposed = attempt->is_disposed;

            if (attempt->value)
            {
                /// Another thread already produced the value while we were acquiring the attempt's mutex.
                ++hits;
                ++concurrent_hits;

                disposer.dispose();

                return {attempt->value, false};
            }
        }

        ++misses;

        /// No try-catch here because it is not needed.
        size_t size = get_size();

        RegionMetadata * region = allocate(size);

        /// Cannot allocate memory, special case.
        if (!region)
        {
            LOG_WARNING(&log, "IGrabberAllocator is full and used, failed to allocate memory");
            return {nullptr, true};
        }

        region->init_key(key);

        {
            total_size_currently_initialized += size;

            try
            {
                region->init_value(std::forward<Init>(initialize));
            }
            catch (...)
            {
                {
                    std::lock_guard global_lock(mutex);
                    freeAndCoalesce(*region); /// Does not need used_regions.
                }

                total_size_currently_initialized.fetch_sub(size, std::memory_order_release);

                throw;
            }
        }

        try
        {
            onSharedValueCreate<true>(*region);

            attempt->value = std::shared_ptr<Value>( //NOLINT: see line 589
                region->value(),
                [this](Value * value) { onValueDelete(value); });

            // init attempt value so other threads, being at line 496, could get the value.

            return {attempt->value, true};
        }
        catch (...)
        {
            std::lock_guard used_lock(used_regions_mutex);

            if (region->TUsedRegionHook::is_linked())
                used_regions.erase(used_regions.iterator_to(*region));

            freeAndCoalesce(*region);

            throw;
        }
    }

/**
 * Get implementation, value deleter for shared_ptr.
 */
private:
    inline ValuePtr getImpl(const Key& key)
    {
        //printf("Thread %lu getImpl\n", pthread_self());

        typename decltype(used_regions)::iterator it;

        {
            std::lock_guard used(used_regions_mutex);

            it = used_regions.find(key, RegionCompareByKey());

            if (used_regions.end() == it)
                return nullptr;
        }

        RegionMetadata& metadata = *it;

        /// Someone decremented metadata refcount to 0 and start deleting from used_regions...
        // can'be now -- onShared locks metadata mutex.

        onSharedValueCreate<false>(metadata);

        return std::shared_ptr<Value>( // NOLINT: not a nullptr
                metadata.value(), [this](Value * ptr){ onValueDelete(ptr); });
    }

    template <bool MayBeInUnused>
    void onSharedValueCreate(RegionMetadata& metadata) noexcept
    {
        std::lock_guard meta_lock(metadata.mutex);
        printf("Thread %lu onSharedValueCreate\n", pthread_self());

        if (++metadata.refcount != 1)
            return;

        printf("Thread %lu onSharedValueCreate, metadata refcount incremented to 1\n", pthread_self());

        {
            std::lock_guard used(used_regions_mutex);
            printf("Thread %lu onSharedValueCreate on %p, pushing to used_regions\n", pthread_self() (void *)metadata.value());

            assert(!metadata.TUsedRegionHook::is_linked());
            used_regions.insert(metadata);

            printf("Thread %lu onSharedValueCreate, pushed to used_regions\n", pthread_self());
        }

        {
            std::lock_guard global(mutex);

            printf("Thread %lu onSharedValueCreate on %p, erasing from unused regions\n", pthread_self() (void *)metadata.value());

            if constexpr (MayBeInUnused)
                if (metadata.TUnusedRegionHook::is_linked())
                    /// May be absent if the region was created by calling allocateFromFreeRegion.
                    unused_regions.erase(unused_regions.iterator_to(metadata));

            // already present in used_regions (in getOrSet), see line 506
            value_to_region.emplace(metadata.value(), &metadata);
        }

        ++metadata.chunk->used_refcount; //atomic here.
        total_size_in_use += metadata.size; // atomic here.
    }

    void onValueDelete(Value * value) noexcept
    {
        printf("Thread %lu onValueDelete on %p\n", 
                pthread_self(), (void * )value);

        RegionMetadata * metadata;

        std::unique_lock<std::mutex> meta_lock; /// 1. empty here

        {
            std::lock_guard global_lock(mutex);

            printf("Thread %lu acquired mutex, 630\n", pthread_self());

            auto it = value_to_region.find(value);

            /// Normally it != value_to_region.end() because there exists at least one shared_ptr using this value (the one
            /// invoking this function), thus value_to_region contains a metadata struct associated with #value.
            assert(it != value_to_region.end());

            metadata = it->second;
            meta_lock = std::unique_lock(metadata->mutex); ///2. init and lock here

            if (--metadata->refcount != 0)
                return;

            printf("Thread %lu onValueDelete on %p, metadata refcount decremented to 0\n", pthread_self(), (void *)value);

            /// Deleting last reference.
            value_to_region.erase(it);

            assert(!metadata->TUnusedRegionHook::is_linked());
            unused_regions.push_back(*metadata);
        }

        --metadata->chunk->used_refcount; //atomic here.
        total_size_in_use -= metadata->size;

        printf("Thread %lu modifying used_regions\n", pthread_self());

        std::lock_guard used(used_regions_mutex);

        printf("Thread %lu onValueDelete, deleting from used_regions\n", pthread_self());

        assert(metadata->TUsedRegionHook::is_linked());
        used_regions.erase(used_regions.iterator_to(*metadata));

        printf("Thread %lu onValueDelete, deleted from used_regions\n", pthread_self());

        /// No delete value here because we do not need to (it will be unmmap'd on MemoryChunk disposal).
    }

    struct InsertionAttemptDisposer;

/**
 * Tokens and attempts
 */
private:
    /**
     * @brief Represents an attempt to insert a new entry into the cache.
     *
     * Useful in 2 situation:
     *
     *  - Value for a given key was not found in the cache, but it may be produced by another thread.
     *    We create this \c attempt and try to achieve a lock on its #mutex.
     *    On success we may find the \c attempt already having a #value (= it was calculated by other thread).
     *  - Same as 1, but the \c attempt has no # value. It simply means value is not present in cache, so we approve
     *    this attempt and insert it.
     */
    struct InsertionAttempt
    {
        constexpr InsertionAttempt(IGrabberAllocator & alloc_) noexcept : alloc(alloc_) {}

        IGrabberAllocator & alloc;

        /// Protects #is_disposed, and #value
        std::mutex mutex;

        /// @c true if this attempt is holding a value.
        bool is_disposed{false};

        /// How many InsertionAttemptDisposer's want to remove this attempt.
        std::atomic_size_t refcount {0};

        ValuePtr value;
    };

    /**
     * @brief Responsible for removing a single InsertionAttempt from the #insertion_attempts container.
     *        The disposal may be performed via an explicit #dispose call or in the destructor.
     *
     * @note Among several concurrent threads the first successful one is responsible for removal.
     *       If they all fail, the last one is responsible.
     */
    struct InsertionAttemptDisposer
    {
        constexpr InsertionAttemptDisposer() noexcept = default;

        ~InsertionAttemptDisposer() noexcept
        {
            std::lock_guard attempt_lock(attempt->mutex);
            dispose();
        }

        const Key * key {nullptr};
        bool attempt_disposed{false};

        std::shared_ptr<InsertionAttempt> attempt;

        /// Now this container is owning the #attempt_ for a given #key_.
        inline void acquire(const Key * key_, const std::shared_ptr<InsertionAttempt> & attempt_) noexcept
        {
            key = key_;
            attempt = attempt_;
            ++attempt->refcount;
        }

        /**
         * @brief Disposes the handled InsertionAttempt if possible.
         */
        void dispose() noexcept
        {
            if (!attempt)
                return;

            if (attempt_disposed)
                return;

            if (attempt->is_disposed)
                return;

            if (--attempt->refcount != 0)
                return;

            std::lock_guard att_lock(attempt->alloc.attempts_mutex);

            attempt->alloc.insertion_attempts.erase(*key);
            attempt->is_disposed = true;
            attempt_disposed = true;
        }
    };

/**
 * Memory-related structures.
 */
private:
    /**
     * @brief Holds a pointer to some allocated space.
     *
     *   - Most low-level primitive of this allocator, represents a sort of std::span of a certain memory part.
     *   - Can hold both free and occupied memory regions.
     *   - mmaps space of desired size in ctor and frees it in dtor.
     *
     * Can be viewed by many IGrabberAllocator::MemoryRegion s (many to one).
     */
    struct MemoryChunk : private boost::noncopyable
    {
        void * ptr;  /// Start of allocated memory.
        size_t size; /// Size of allocated memory.

        /**
         * @brief How many RegionMetadatas being in #used_regions reference this chunk.
         *
         * Must track due to existence of IGrabberAllocator::shrinkToFit method.
         *
         * @see IGrabberAllocator::shrinkToFit
         */
        std::atomic_size_t used_refcount;

        constexpr MemoryChunk(size_t size_, void * address_hint)
            : size(size_), used_refcount(0)
        {
            CurrentMemoryTracker::alloc(size_);

            ptr = mmap(
                __builtin_assume_aligned(address_hint, ValueAlignment),
                size,
                PROT_READ | PROT_WRITE,
                MAP_PRIVATE |
                MAP_ANONYMOUS
#ifdef _MAP_POPULATE_AVAILABLE
                | MAP_POPULATE /*possible speedup on read-ahead*/
#endif
                ,
                /**fd */ -1,
                /**offset*/ 0);

            if (MAP_FAILED == ptr)
                DB::throwFromErrno(
                    "Allocator: Cannot mmap " +
                    formatReadableSizeWithBinarySuffix(size) + ".",
                    DB::ErrorCodes::CANNOT_ALLOCATE_MEMORY);
        }

        ~MemoryChunk()
        {
            if (ptr && munmap(ptr, size))
                DB::throwFromErrno(
                    "Allocator: Cannot munmap " +
                    formatReadableSizeWithBinarySuffix(size) + ".",
                    DB::ErrorCodes::CANNOT_MUNMAP);

            CurrentMemoryTracker::free(size);
        }

        constexpr MemoryChunk(MemoryChunk && other) noexcept
            : ptr(other.ptr), size(other.size), used_refcount(other.used_refcount)
        {
            other.ptr = nullptr;
        }
    };

    /**
     * @brief Element referenced in all intrusive containers here.
     *        Includes a #Key-#Value pair and a pointer to some IGrabberAllocator::MemoryChunk part storage
     *        holding some #Value's data.
     *
     * Consider #Value = std::vector of size 10.
     * The memory layout is something like this:
     *
     * (Heap, allocated by ::new()) Region metadata { void *ptr, std::vector Value;}, ptr = value.data = 0xcafebabe.
     * (Head, allocated by mmap()) MemoryChunk {void * ptr}, ptr = 0xcafebabe
     * (0xcafebabe) [Area with std::vector data]
     */
    struct RegionMetadata :
        public TUnusedRegionHook,
        public TAllRegionsHook,
        public TFreeRegionHook,
        public TUsedRegionHook
    {
        /// #Key and #Value needn't be default-constructible.

        std::aligned_storage_t<sizeof(Key), alignof(Key)> key_storage;
        std::aligned_storage_t<sizeof(Value), alignof(Value)> value_storage;

        std::mutex mutex; //protects refcount, see onValueDelete and onSharedValueCreate

        /// No need for std::optional
        bool key_initialized{false};
        bool value_initialized{false};

        union
        {
            /// Pointer to a IGrabberAllocator::MemoryChunk part storage for a given region.
            void * ptr {nullptr};

            /// Used in IGrabberAllocator::freeAndCoalesce.
            char * char_ptr;
        };

        /// Size of storage starting at #ptr.
        size_t size {0};

        /// How many outer users reference this object's #value?
        size_t refcount {0};

        /**
         * Used to compare regions (usually MemoryChunk.ptr) and update MemoryChunk's used_refcount
         * @see IGrabberAllocator::evict
         * @see IGrabberAllocator::onValueDelete
         * @see IGrabberAllocator::onSharedValueCreate
         */
        MemoryChunk * chunk {nullptr};

        [[nodiscard]] static RegionMetadata * create() { return new RegionMetadata(); }

        constexpr void destroy() noexcept {
            if (key_initialized)
                std::destroy_at(std::launder(reinterpret_cast<Key*>(&key_storage)));

            if (value_initialized)
                std::destroy_at(std::launder(reinterpret_cast<Value*>(&value_storage)));

            delete this;
        }

        /**
         * @brief Tries to initialize the key.
         *
         * @note Exceptions occuring in Key's const lvalue ctor will be propagated to the caller.
         * @note It is the caller's responsibility to ensure this method is called on uninitialized key.
         */
        constexpr void init_key(const Key& key)
        {
            /// TODO Replace with std version
            ga::construct_at<Key>(&key_storage, key);
            key_initialized = true;
        }

        /**
         * @brief Tries to initialize the value.
         *
         * @note Exceptions occuring in Value's rvalue ctor will be propagated to the caller.
         * @note It is the caller's responsibility to ensure this method is called on uninitialized value.
         */
        template <class Init>
        constexpr void init_value(Init&& init_func)
        {
            /// TODO Replace with std version
            ga::construct_at<Value>(&value_storage, init_func(ptr));
            value_initialized = true;
        }

        /**
         * @brief Tries to approach the key.
         * @note It is the caller's responsibility to ensure this method is called on initialized key.
         */
        constexpr const Key& key() const noexcept
        {
            return *std::launder(reinterpret_cast<const Key*>(&key_storage));
        }

        /**
         * @brief Tries to approach the value.
         * @note It is the caller's responsibility to ensure this method is called on initialized value.
         */
        constexpr Value * value() noexcept
        {
            return std::launder(reinterpret_cast<Value*>(&value_storage));
        }

        [[nodiscard, gnu::pure]] constexpr bool operator< (const RegionMetadata & other) const noexcept
        {
            return size < other.size;
        }

        [[nodiscard, gnu::pure]] constexpr bool isFree() const noexcept { return TFreeRegionHook::is_linked(); }

    private:
        RegionMetadata() {}
        ~RegionMetadata() = default;
    };

    static constexpr auto region_metadata_disposer = [](RegionMetadata * ptr) { ptr->destroy(); };

    struct RegionCompareBySize
    {
        constexpr bool operator() (const RegionMetadata & a, const RegionMetadata & b) const noexcept { return a.size < b.size; }
        constexpr bool operator() (const RegionMetadata & a, size_t size) const noexcept { return a.size < size; }
        constexpr bool operator() (size_t size, const RegionMetadata & b) const noexcept { return size < b.size; }
    };

    struct RegionCompareByKey
    {
        constexpr bool operator() (const RegionMetadata & a, const RegionMetadata & b) const noexcept { return a.key() < b.key(); }
        constexpr bool operator() (const RegionMetadata & a, Key key) const noexcept { return a.key() < key; }
        constexpr bool operator() (Key key, const RegionMetadata & b) const noexcept { return key < b.key(); }
    };

/**
 * Memory modifying operations.
 */
private:
    /**
     * @brief Main allocation routine. Allocates from free region (possibly creating a new one).
     * @note Method does not insert allocated region to #used_regions or #unused_regions.
     * @return Desired region or std::nullptr.
     */
    inline RegionMetadata * allocate(size_t size)
    {
        std::lock_guard global_lock(mutex); /// Allocate does not need used_regions

        size = ga::roundUp(size, ValueAlignment);

        if (auto it = free_regions.lower_bound(size, RegionCompareBySize()); free_regions.end() != it)
            return allocateFromFreeRegion(*it, size);

        /// If nothing was found and total size of allocated chunks plus required size is lower than maximum,
        /// allocate from a newly created region spanning through the chunk.
        if (size_t req = std::max(MinChunkSize, ga::roundUp(size, page_size)); total_chunks_size + req <= max_cache_size)
            return allocateFromFreeRegion(*addNewChunk(req), size);

        /// Evict something from cache and continue.
        while (true)
        {
            RegionMetadata * res = evict(size);

            /// Nothing to evict. All cache is full and in use - cannot allocate memory.
            if (!res)
                return nullptr;

            /// Not enough. Evict more.
            if (res->size < size)
                continue;

            return allocateFromFreeRegion(*res, size);
        }
    }

    /**
     * @brief Given a #free_region, occupies its part of size #size.
     *
     * Modifies #free_regions, #all_regions.
     */
    constexpr RegionMetadata * allocateFromFreeRegion(RegionMetadata & free_region, size_t size)
    {
        if (free_region.size < size) return nullptr;

        ++allocations;
        allocated_bytes += size;

        if (free_region.size == size)
        {
            total_allocated_size += size;
            free_regions.erase(free_regions.iterator_to(free_region));
            return &free_region;
        }

        /// No try-catch needed
        RegionMetadata * allocated_region = RegionMetadata::create();

        total_allocated_size += size;

        allocated_region->ptr = free_region.ptr;
        allocated_region->chunk = free_region.chunk;
        allocated_region->size = size;

        free_regions.erase(free_regions.iterator_to(free_region));

        /// chop the beginning of current region of size size.
        free_region.size -= size;

        /// shift current region's start.
        free_region.char_ptr += size;

        /// return the region [start + size; end] to free regions
        free_regions.insert(free_region);

        all_regions.insert(all_regions.iterator_to(free_region), *allocated_region);

        return allocated_region;
    }

    /**
     * @brief Allocates a MemoryChunk of specified size.
     * @return A free region, spanning through the whole allocated chunk.
     * @post #all_regions and #free_regions contain target region.
     *
     * Modifies #free_regions, #all_regions.
     */
    constexpr RegionMetadata * addNewChunk(size_t size)
    {
        chunks.emplace_back(size, ASLR());
        MemoryChunk & chunk = chunks.back();

        total_chunks_size += size;

        RegionMetadata * free_region;

        try
        {
            free_region = RegionMetadata::create();
        }
        catch (...)
        {
            total_chunks_size -= size;
            chunks.pop_back();
            throw;
        }

        free_region->ptr = chunk.ptr;
        free_region->chunk = &chunk;
        free_region->size = chunk.size;

        all_regions.push_back(*free_region);
        free_regions.insert(*free_region);

        return free_region;
    }

    /**
     * @brief Evicts region of at least #requested size from cache and returns it, probably coalesced with nearby free
     *        regions. While size is not enough, evicts adjacent regions at right, if any.
     *
     * @post Target region is removed from #unused_regions and #used_regions and inserted into #free_regions.
     *
     * @return std::nullptr if there are no unused regions.
     * @return Target region otherwise.
     */
    RegionMetadata * evict(size_t requested) noexcept
    {
        if (unused_regions.empty())
            return nullptr;

        auto region_it = all_regions.iterator_to(unused_regions.front());

        while (true)
        {
            RegionMetadata & evicted = *region_it;

            total_allocated_size -= evicted.size;

            unused_regions.erase(unused_regions.iterator_to(evicted));

            {
                std::lock_guard used_lock(used_regions_mutex);

                if (evicted.TUsedRegionHook::is_linked())
                    used_regions.erase(used_regions.iterator_to(evicted));
            }

            ++evictions;
            evicted_bytes += evicted.size;

            freeAndCoalesce(evicted);

            if (evicted.size >= requested)
                return &evicted;

            ++region_it;

            if (region_it == all_regions.end() ||
                region_it->chunk != evicted.chunk ||
                !region_it->TUnusedRegionHook::is_linked())
                return &evicted;

            ++secondary_evictions;
        }
    }

    /**
     * @brief Inserts #region into #free_regions container, probably coalescing it with adjacent free regions (one to
     *        the right and one to the left).
     *
     * @pre #region must not be present in #free_regions, #used_regions and #unused_regions.
     *
     * @param region Target region to insert.
     */
    constexpr void freeAndCoalesce(RegionMetadata & region) noexcept
    {
        auto target_region_it = all_regions.iterator_to(region);
        auto region_it = target_region_it;

        auto erase_free_region = [this, &region](auto region_iter) noexcept {
            if (region_iter->chunk != region.chunk || !region_iter->isFree())
                return;

            region.size += region_iter->size;
            region.char_ptr-= region_iter->size;

            free_regions.erase(free_regions.iterator_to(*region_iter));
            all_regions.erase_and_dispose(region_iter, region_metadata_disposer);
        };

        /// Coalesce with left neighbour
        if (all_regions.begin() != region_it)
            erase_free_region(--region_it);

        region_it = target_region_it;

        /// Coalesce with right neighbour
        if (all_regions.end() != ++region_it)
            erase_free_region(region_it);

        free_regions.insert(region);
    }
};

}






using namespace DB;

using Alloc = IGrabberAllocator<int, int>;

struct Holder
{
    Holder(Alloc& a, int key)
    {
        ptr = a.getOrSet(key,
            []{ return sizeof(int); },
            [key](void *){ return key; }).first;
    }

    std::shared_ptr<int> ptr;
};

int main() noexcept
{
   Alloc cache(MMAP_THRESHOLD);

   std::vector<std::thread> thread_pool;

   for (size_t k = 0; k < 2; k++)
       thread_pool.emplace_back([&cache] {
           for (int i = 1; i < 10000; ++i) {
               Holder inc(cache, 1);
           }});

   for (auto& t : thread_pool) t.join();
}

