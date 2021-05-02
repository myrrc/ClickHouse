#pragma once

#include <atomic>
#include <filesystem>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>
#include <unordered_map>
#include <Poco/Logger.h>

#include "common/logger_useful.h"
#include <common/types.h>

#include <Common/SymbolIndex.h>
#include <Common/Dwarf.h>
#include <Common/ThreadPool.h>

namespace detail
{
using namespace DB;

using Addr = void *;
using Addrs = std::vector<Addr>;

struct SourceFileData
{
    std::unordered_map<Addr, size_t /* call count */> functions_hit;
    std::unordered_map<size_t /* line */, size_t /* call_count */> lines_hit;
};

struct SourceFileInfo
{
    std::string path;

    using InstrumentedFunctions = std::vector<Addr>;
    InstrumentedFunctions instrumented_functions;

    std::vector<size_t> instrumented_lines;

    explicit SourceFileInfo(const std::string& path_)
        : path(path_), instrumented_functions(), instrumented_lines() {}
};

struct SourceLocation
{
    std::string full_path;
    size_t line;
};

using SourceFilePathIndex = size_t;

struct AddrInfo
{
    size_t line;
    SourceFilePathIndex index;
};

struct FunctionInfo
{
    std::string_view name;
    size_t start_line;
    SourceFilePathIndex index;
};

using TestData = std::vector<SourceFileData>; // vector index = source_file_paths index

class Writer
{
public:
    static inline Writer& instance()
    {
        static Writer w;
        return w;
    }

    /// Called when class needs to store all instrumented addresses.
    void initializePCTable(const uintptr_t *pcs_beg, const uintptr_t *pcs_end);

    /// Called when guard variables for all instrumented edges have been initialized.
    inline void initializedGuards(uint32_t count) { edges.reserve(count); }

    /// Before server has initialized, we can't log data to Poco.
    inline void serverHasInitialized()
    {
        base_log = &Poco::Logger::get(std::string{logger_base_name});
        symbolizeAllInstrumentedAddrs();
    }

    /// Called when a critical edge in binary is hit.
    void hit(void * addr);

private:
    Writer();

    static constexpr const std::string_view logger_base_name = "Coverage";
    static constexpr const std::string_view coverage_dir_relative_path = "../../coverage";

    /// How many tests are converted to LCOV in parallel.
    static constexpr const size_t thread_pool_test_processing = 10;

    /// How may threads concurrently symbolize the addresses on binary startup.
    static constexpr const size_t thread_pool_symbolizing = 16;

    /// How many addresses do we dump into local storage before acquiring the edges_mutex and pushing into edges.
    static constexpr const size_t hits_batch_array_size = 100000;

    static thread_local inline size_t hits_batch_index = 0; /// How many addresses are currently in the local storage.
    static thread_local inline std::array<void*, hits_batch_array_size> hits_batch_storage{};

    const Poco::Logger * base_log; /// do not use the logger before call of serverHasInitialized.

    const std::filesystem::path coverage_dir;

    const MultiVersion<SymbolIndex>::Version symbol_index;
    const Dwarf dwarf;

    FreeThreadPool pool;

    Addrs edges;
    std::optional<std::string> test;
    std::mutex edges_mutex; // protects test, edges

    using NameToIndexCache = std::unordered_map<std::string, SourceFilePathIndex>;
    using SourceFileCache = std::vector<SourceFileInfo>;

    NameToIndexCache source_file_name_to_path_index;
    SourceFileCache source_files_cache;

    std::unordered_map<Addr, AddrInfo> addr_cache;

    using FunctionCache = std::unordered_map<Addr, FunctionInfo>;
    FunctionCache function_cache;

    Addrs pc_table_addrs;
    Addrs pc_table_function_entries;

    inline SourceLocation getSourceLocation(const void * virtual_addr) const
    {
        /// This binary gets loaded first, so binary_virtual_offset = 0
        const Dwarf::LocationInfo loc = dwarf.findAddressForCoverageRuntime(uintptr_t(virtual_addr));
        return {loc.file.toString(), loc.line};
    }

    inline std::string_view symbolize(const void * virtual_addr) const
    {
        return symbol_index->findSymbol(virtual_addr)->name;
    }

    void dumpAndChangeTestName(std::string_view test_name);

    struct TestInfo
    {
        std::string_view name;
        const Poco::Logger * log;
    };

    void prepareDataAndDump(TestInfo test_info, const Addrs& addrs);
    void convertToLCOVAndDump(TestInfo test_info, const TestData& test_data);

    /// Fills addr_cache, function_cache, source_files_cache, source_file_name_to_path_index
    /// Clears pc_table_addrs, pc_table_function_entries.
    void symbolizeAllInstrumentedAddrs();

    struct FuncAddrTriple
    {
        size_t line;
        void * addr;
        std::string_view name;

        FuncAddrTriple(size_t line_, void * addr_, std::string_view name_): line(line_), addr(addr_), name(name_) {}
    };

    struct AddrPair
    {
        size_t line;
        void * addr;

        AddrPair(size_t line_, void * addr_): line(line_), addr(addr_) {}
    };

    using FuncLocalCache = std::unordered_map<std::string /*source file path*/, std::vector<FuncAddrTriple>>;
    using AddrLocalCache = std::unordered_map<std::string, std::vector<AddrPair>>;

    template <class Cache> using CachesArr = std::array<Cache, thread_pool_symbolizing>;

    void mergeFunctionDataToCaches(const CachesArr<FuncLocalCache>& data);
    void mergeAddressDataToCaches(const CachesArr<AddrLocalCache>& data);

    template <class Cache, bool is_func_cache>
    void scheduleSymbolizationJobs(const CachesArr<Cache>& caches, const Addrs& addrs)
    {
        constexpr auto pool_size = thread_pool_symbolizing;
        const size_t step = addrs.size() / pool_size;

        auto begin = addrs.cbegin();
        auto r_end = addrs.cend();

        for (size_t thread_index = 0; thread_index < pool_size; ++thread_index)
            pool.scheduleOrThrowOnError([&] // =, this won't do what expected, so copy everth by ref.
            {
                const auto start = begin + thread_index * step;
                const auto end = thread_index == pool_size - 1
                    ? r_end
                    : start + step;

                const Poco::Logger * const log = &Poco::Logger::get(
                    fmt::format("{}.func{}", logger_base_name, thread_index));

                const size_t size = end - start;

                auto& cache = caches[thread_index];

                time_t elapsed = time(nullptr);

                for (auto it = start; it != end; ++it)
                {
                    Addr addr = *it;

                    const SourceLocation source = getSourceLocation(addr);

                    if constexpr (is_func_cache)
                    {
                        if (auto cache_it = cache.find(source.full_path); cache_it != cache.end())
                            cache_it->second.emplace_back(source.line, addr, symbolize(addr));
                        else
                            cache[source.full_path] = {{source.line, addr, symbolize(addr)}};
                    }
                    else
                    {
                        if (auto cache_it = cache.find(source.full_path); cache_it != cache.end())
                            cache_it->second.emplace_back(source.line, addr);
                        else
                            cache[source.full_path] = {{source.line, addr}};
                    }

                    if (time_t current = time(nullptr); current > elapsed)
                    {
                        LOG_INFO(log, "Processed {}/{} functions", it - start, size);
                        elapsed = current;
                    }
                }
            });
    }
};
}
