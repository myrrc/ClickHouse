#include "Coverage.h"

#include <unistd.h>

#include <memory>
#include <utility>
#include <thread>

#include <fmt/core.h>
#include <fmt/format.h>

#include "common/logger_useful.h"

#include "Common/ProfileEvents.h"
#include "Common/ErrorCodes.h"
#include "Common/Exception.h"

namespace DB::ErrorCodes
{
    extern const int CANNOT_OPEN_FILE;
    extern const int FILE_ALREADY_EXISTS;
    extern const int LOGICAL_ERROR;
}

namespace coverage
{
static const size_t hardware_concurrency { std::thread::hardware_concurrency() };
static const String logger_base_name {"Coverage"};

using Exception = DB::Exception;

namespace
{
// Unused in Darwin and FreeBSD builds.
[[maybe_unused]] inline SymbolIndexInstance getInstanceAndInitGlobalCounters()
{
    /**
     * Writer is a singleton, so it initializes statically.
     * SymbolIndex uses a MMapReadBufferFromFile which uses ProfileEvents.
     * If no thread was found in the events profiler, a global variable global_counters is used.
     *
     * This variable may get initialized after Writer (static initialization order fiasco).
     * In fact, __sanitizer_cov_trace_pc_guard_init is called before global_counters init.
     *
     * We can't use constinit on that variable as it has a shared_ptr on it, so we just
     * ultimately initialize it before getting the instance.
     *
     * We can't initialize global_counters in ProfileEvents.cpp to nullptr as in that case it will become nullptr.
     * So we just initialize it twice (here and in ProfileEvents.cpp).
     */
    ProfileEvents::global_counters = ProfileEvents::Counters(ProfileEvents::global_counters_array);

    return SymbolIndex::instance();
}
}

// void TaskQueue::start()
// {
//     worker = std::thread([this]
//     {
//         while (true)
//         {
//             Task task;
// 
//             {
//                 std::unique_lock lock(mutex);
// 
//                 task_or_shutdown.wait(lock, [this] { return shutdown || !tasks.empty(); });
// 
//                 if (tasks.empty())
//                     return;
// 
//                 task = std::move(tasks.front());
//                 tasks.pop();
//             }
// 
//             task();
//         }
//     });
// }
// 
// void TaskQueue::wait()
// {
//     size_t active_tasks = 0;
// 
//     {
//         std::lock_guard lock(mutex);
//         active_tasks = tasks.size();
//         shutdown = true;
//     }
// 
//     // If queue has n tasks left, we need to notify it n times
//     // (to process all tasks) and one extra time to shut down.
//     for (size_t i = 0; i < active_tasks + 1; ++i)
//         task_or_shutdown.notify_one();
// 
//     if (worker.joinable())
//         worker.join();
// }

Writer::Writer()
    :
#if NON_ELF_BUILD
      symbol_index(),
      dwarf()
#else
      symbol_index(getInstanceAndInitGlobalCounters()),
      dwarf(symbol_index->getSelf()->elf)
#endif
    {}

Writer& Writer::instance()
{
    static Writer w;
    return w;
}

void Writer::pcTableCallback(const Addr * start, const Addr * end)
{
    const size_t bb_pairs = start - end;
    bb_count = bb_pairs / 2;

    instrumented_blocks_addrs.resize(bb_count);
    instrumented_blocks_start_lines.resize(bb_count);

    for (size_t i = 0; i < bb_count; i += 2)
        /// Real address is the previous instruction, see implementation in clang:
        /// https://github.com/llvm/llvm-project/blob/main/llvm/tools/sancov/sancov.cpp#L768
        instrumented_blocks_addrs[i] = start[i] - 1;
}

void Writer::countersCallback(bool * start, bool * end)
{
    std::fill(start, end, false);
    current = {start, end};
}

void Writer::deinitRuntime()
{
    if (is_client)
        return;

    report_file.close();

    LOG_INFO(base_log, "Shut down runtime");
}

void Writer::onClientInitialized()
{
    is_client = true;
}

void Writer::onServerInitialized()
{
    // We can't log data using Poco before server initialization as Writer constructor gets called in
    // sanitizer callback which occurs before Poco internal structures initialization.
    base_log = &Poco::Logger::get(logger_base_name);

    // Some functional .sh tests spawn own server instances.
    // In coverage mode it leads to concurrent file writes (file write + open in "w" truncate mode, to be precise),
    // which results in data corruption.
    // To prevent such situation, target file is not allowed to exist at server start.
    if (access(report_path.c_str(), F_OK) == 0)
        throw Exception(DB::ErrorCodes::FILE_ALREADY_EXISTS, "Report file {} already exists", report_path);

    // fwrite also can't be called before server initialization (some internal state is left uninitialized if we
    // try to write file in PC table callback).
    if (report_file.set(report_path, "w") == nullptr)
        throw Exception(DB::ErrorCodes::CANNOT_OPEN_FILE,
            "Failed to open {} in write mode: {}", report_path, strerror(errno));
    else
        LOG_INFO(base_log, "Opened report file {}", report_path);

    //tasks_queue.start();

    symbolizeInstrumentedData();
}

void Writer::symbolizeInstrumentedData()
{
    LocalCaches caches(hardware_concurrency);

    LOG_INFO(base_log, "{} instrumented basic blocks. Using thread pool of size {}", bb_count, hardware_concurrency);

    symbolizeAddrsIntoLocalCaches(caches);
    mergeIntoGlobalCache(caches);

    if (const size_t sf_count = source_files.size(); sf_count < 1000)
        throw Exception(DB::ErrorCodes::LOGICAL_ERROR,
            "Not enough source files ({} < 1000), must be a symbolizer bug", sf_count);
    else
        LOG_INFO(base_log, "Found {} source files", sf_count);

    writeReportHeader();

    // Testing script in docker (/docker/test/coverage/run.sh) waits for this message and starts tests afterwards.
    // If we place it before function return, concurrent writes to report file will happen and data will corrupt.
    LOG_INFO(base_log, "Symbolized all addresses");
}

void Writer::symbolizeAddrsIntoLocalCaches(LocalCaches& caches)
{
    // Each worker symbolizes an address range. Ranges are distributed uniformly.

    std::vector<std::thread> workers;
    workers.reserve(hardware_concurrency);

    const size_t step = bb_count / hardware_concurrency;

    for (size_t thread_index = 0; thread_index < hardware_concurrency; ++thread_index)
        workers.emplace_back([this, step, thread_index, &cache = caches[thread_index]]
        {
            const size_t start_index = thread_index * step;
            const size_t end_index = std::min(start_index + step, bb_count - 1);

            const Poco::Logger * log = &Poco::Logger::get(fmt::format("{}.{}", logger_base_name, thread_index));

            for (size_t i = start_index; i < end_index; ++i)
            {
                const BBIndex bb_index = static_cast<BBIndex>(i);
                const Dwarf::LocationInfo loc = dwarf.findAddressForCoverageRuntime(instrumented_blocks_addrs[i]);
                const SourcePath src_path = loc.file.toString();

                if (src_path.empty())
                    throw Exception(DB::ErrorCodes::LOGICAL_ERROR, "Internal symbolizer error");

                if (i % 4096 == 0)
                    LOG_INFO(log, "{}/{}, file: {}", i - start_index, end_index - start_index, src_path);

                const Line line = static_cast<Line>(loc.line);

                if (auto cache_it = cache.find(src_path); cache_it != cache.end())
                    cache_it->second.push_back(IndexAndLine{bb_index, line});
                else
                    cache[src_path] = {{bb_index, line}};
            }
        });

    for (auto & worker : workers)
        if (worker.joinable())
            worker.join();
}

void Writer::mergeIntoGlobalCache(const LocalCaches& caches)
{
    std::unordered_map<SourcePath, SourceIndex> path_to_index;

    for (const auto& cache : caches)
        for (const auto& [source_path, symbolized_data] : cache)
        {
            SourceIndex source_index;

            if (auto it = path_to_index.find(source_path); it != path_to_index.end())
                source_index = it->second;
            else
            {
                source_index = path_to_index.size();
                path_to_index.emplace(source_path, source_index);
                source_files.push_back(SourceInfo{source_path});
            }

            Blocks& instrumented = source_files[source_index].instrumented_blocks;

            for (const auto [bb_index, start_line] : symbolized_data)
            {
                instrumented_blocks_start_lines[bb_index] = start_line;
                instrumented.push_back(bb_index);
            }
        }
}

void Writer::onChangedTestName(String old_test_name)
{
    // String is passed by value as it's swapped
    // Note: this function slows down setSetting, so it should be as fast as possible.

    if (is_client)
        return;

    old_test_name.swap(test_name); // now old_test_name contains finished test name, test_name contains next test name

    if (old_test_name.empty()) // Processing first test
        return;

    report_file.write(Magic::TestEntry);
    report_file.write(old_test_name);

    for (size_t i = 0; i < bb_count; ++i)
        if (current[i])
        {
            current[i] = false;
            report_file.write(i);
        }

    if (test_name.empty()) // Finished testing
        deinitRuntime();
}

void Writer::writeReportHeader()
{
    report_file.write(Magic::ReportHeader);
    report_file.write(source_files.size());

    for (const SourceInfo& file : source_files)
    {
        report_file.write(file.path);
        report_file.write(file.instrumented_blocks.size());

        for (BBIndex index : file.instrumented_blocks)
        {
            report_file.write(index);
            report_file.write(instrumented_blocks_start_lines[index]);
        }
    }
}
}
