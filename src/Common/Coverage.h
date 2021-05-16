#pragma once

#include <cstdio>

#include <algorithm>
#include <atomic>
#include <exception>
#include <filesystem>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <queue>
#include <list>
#include <string_view>
#include <type_traits>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <fmt/format.h>
#include <Poco/Logger.h>

#include "common/logger_useful.h"
#include <common/types.h>

#include <Common/SymbolIndex.h>
#include <Common/Dwarf.h>

namespace detail
{
using namespace DB;

using EdgeIndex = uint32_t;
using Addr = void *;

using CallCount = size_t;
using Line = size_t;

/**
 * Simplified FreeThreadPool from Common/ThreadPool.h .
 * Not intended for general use (some invariants broken).
 *
 * - Does not use job priorities.
 * - Does not throw.
 * - Does not use metrics.
 * - Does not remove threads after job finish, max_threads run all the time.
 * - Spawns max_threads on startup.
 * - Invokes job with job index.
 * - Some operations are not thread-safe.
 *
 * Own implementation needed as Writer does not use most FreeThreadPool features (and we can save ~20 minutes by
 * using this class instead of the former).
 */
class TaskQueue
{
public:
    explicit inline TaskQueue(size_t max_threads_): max_threads(max_threads_)
    {
        for (size_t i = 0; i < max_threads; ++i)
            spawnThread(i);
    }

    template <class J>
    void schedule(J && job)
    {
        {
            std::unique_lock lock(mutex);
            jobs.emplace(std::forward<J>(job));
            ++scheduled_jobs;
        }

        new_job_or_shutdown.notify_one();
    }

    inline void wait()
    {
        std::unique_lock lock(mutex);
        job_finished.wait(lock, [this] { return scheduled_jobs == 0; });
    }

    inline ~TaskQueue() { finalize(); }

    inline void changePoolSizeAndRespawnThreads(size_t size)
    {
        // will be called when there are 0 jobs active, no mutex needed
        //std::lock_guard lock(mutex);

        finalize();
        shutdown = false;

        max_threads = size;

        for (size_t i = 0; i < max_threads; ++i)
            spawnThread(i);
    }

    void finalize();

private:
    using Job = std::function<void(size_t)>;
    using Thread = std::thread;

    std::mutex mutex;
    std::condition_variable job_finished;
    std::condition_variable new_job_or_shutdown;

    size_t max_threads;

    size_t scheduled_jobs = 0;
    bool shutdown = false;

    std::queue<Job> jobs;
    std::list<Thread> threads;
    using Iter = typename std::list<Thread>::iterator;

    void worker(size_t thread_id);

    inline void spawnThread(size_t thread_index)
    {
        threads.emplace_front([this, thread_index] { worker(thread_index); });
    }
};

/// RAII wrapper for FILE *
class FileWrapper
{
    FILE * handle;
public:
    inline FileWrapper(): handle(nullptr) {}
    inline void set(const std::string& pathname, const char * mode) { handle = fopen(pathname.data(), mode); }
    inline FILE * file() { return handle; }
    inline void close() { fclose(handle); handle = nullptr; }
    inline void write(const fmt::memory_buffer& mb) { fwrite(mb.data(), 1, mb.size(), handle); }
};

/**
 * Custom code coverage runtime for -WITH_COVERAGE=1 build.
 *
 * On startup, symbolizes all instrumented addresses in binary (information provided by clang).
 * During testing, handles calls from sanitizer callbacks (Common/CoverageCallbacks.h).
 * Writes a report in CCR format (docs/development/coverage.md) after testing.
 */
class Writer
{
public:
    static constexpr std::string_view setting_test_name = "coverage_test_name";
    static constexpr std::string_view setting_tests_count = "coverage_tests_count";
    static constexpr std::string_view setting_report_path = "coverage_report_path";

    static Writer& instance();

    void initializeRuntime(const uintptr_t *pc_array, const uintptr_t *pc_array_end);

    void onServerInitialized();

    void hit(EdgeIndex edge_index);

    inline void setTestsCount(size_t tests_count)
    {
        // This is an upper bound (as we don't know which tests will be skipped before running them).
        tests.resize(tests_count);
    }

    inline void setReportPath(const std::string& report_path_)
    {
        report_path = report_path_;
        report_file.set(report_path, "w");

        LOG_INFO(base_log, "Set report file path to {}, writing CCR header, {}b",
                report_path_, ccr_header_buffer.size());

        writeCCRHeaderToFile();
    }

    void onChangedTestName(std::string old_test_name); // String is passed by value as it's swapped with _test_.

private:
    Writer();

    static constexpr std::string_view logger_base_name = "Coverage";

    /// Each test duration is longer than coverage test processing pipeline (converting and dumping to disk), so no
    /// more than 1 thread is needed
    static constexpr size_t thread_pool_size_for_tests = 1;

    static constexpr size_t total_source_files_hint = 5000; // each LocalCache pre-allocates this / pool_size.

    const size_t hardware_concurrency;
    const Poco::Logger * base_log;

    const MultiVersion<SymbolIndex>::Version symbol_index;
    const Dwarf dwarf;

    const std::filesystem::path clickhouse_src_dir_abs_path;
    std::filesystem::path report_path;

    size_t functions_count;
    size_t addrs_count;
    size_t edges_count;

    TaskQueue tasks_queue;

    using SourceRelativePath = std::string; // from ClickHouse src/ directory

    struct SourceFileInfo
    {
        SourceRelativePath relative_path;
        std::vector<EdgeIndex> instrumented_functions;
        std::vector<Line> instrumented_lines;

        explicit SourceFileInfo(std::string path_)
            : relative_path(std::move(path_)), instrumented_functions(), instrumented_lines() {}
    };

    using SourceFileIndex = size_t;

    struct EdgeInfo
    {
        Line line;
        SourceFileIndex index;
        std::string_view name;

        constexpr bool isFunctionEntry() const { return !name.empty(); }
    };

    struct TestData
    {
        struct SourceData
        {
            std::unordered_map<EdgeIndex, CallCount> functions_hit;
            std::unordered_map<Line, CallCount> lines_hit;
        };

        std::string name; // If is empty, there is no data for test.
        size_t test_index;
        const Poco::Logger * log;
        std::vector<SourceData> data; // [i] means source file in source_files_cache with index i.
    };

    using EdgesHit = std::vector<EdgeIndex>;

    std::vector<Addr> edges_to_addrs;
    std::vector<bool> edge_is_func_entry;

    std::vector<SourceFileInfo> source_files_cache;
    std::unordered_map<SourceRelativePath, SourceFileIndex> source_rel_path_to_index;

    std::vector<EdgeInfo> edges_cache;

    std::vector<TestData> tests; /// Data accumulated for all tests.
    EdgesHit edges_hit; /// Data accumulated for a single test.

    size_t test_index;
    FileWrapper report_file;

    void deinitRuntime();

    void prepareDataAndDump(TestData& test_data, const EdgesHit& hits);

    // CCR (ClickHouse coverage report) is a format for storing large coverage reports while preserving per-test data.

    fmt::memory_buffer ccr_header_buffer; // Report header may be formatted before report file path is specified.

    inline void writeCCRHeaderToFile()
    {
        report_file.write(ccr_header_buffer);
        // do not free buffer memory to save a bit of time.
        fflush(report_file.file());
    }

    void writeCCRHeaderToInternalBuffer();
    void writeCCREntry(const TestData& test_data);
    void writeCCRFooter();

    // Symbolization

    void symbolizeInstrumentedData();

    template <class CacheItem>
    using LocalCache = std::unordered_map<SourceRelativePath, typename CacheItem::Container>;

    template <class CacheItem>
    using LocalCaches = std::vector<LocalCache<CacheItem>>;

    /// Spawn workers symbolizing addresses obtained from PC table to internal local caches.
    template <bool is_func_cache, class CacheItem>
    void scheduleSymbolizationJobs(LocalCaches<CacheItem>& local_caches, const std::vector<EdgeIndex>& data);

    /// Merge local caches' symbolized data [obtained from multiple threads] into global caches.
    template<bool is_func_cache, class CacheItem>
    void mergeDataToCaches(const LocalCaches<CacheItem>& data);
};
}
