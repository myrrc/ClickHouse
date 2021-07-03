#pragma once

#include <span>
#include <unordered_map>
#include <Poco/Logger.h>
#include <common/types.h>
#include "CoverageDecls.h"

namespace coverage
{
static const String report_path { "/report.ccr" }; // Change if you want to test runtime outside of Docker
static constexpr std::string_view setting_test_name = "coverage_test_name";

using Addr = uintptr_t;
using Line = int;

using SourceIndex = int;
using BBIndex = int;
using AddrIndex = int;
using TestIndex = int;

using SourcePath = String;

using Blocks = std::vector<BBIndex>;
using SourceInfo = std::pair<SourcePath, Blocks /* instrumented blocks */>;

class Writer
{
public:
    static Writer& instance();

    void pcTableCallback(const Addr * start, const Addr * end) noexcept;
    void countersCallback(bool * start, bool * end) noexcept;

    void onServerInitialized();
    void onClientInitialized() noexcept;
    void onChangedTestName(String name);

private:
    Writer();

    const Poco::Logger * base_log {nullptr};

    [[maybe_unused]] const SymbolIndexInstance symbol_index; // Unused in Darwin and FreeBSD build
    const Dwarf dwarf;

    // CH client is usually located inside main CH binary, but we don't need to instrument client code.
    // This variable is set on client initialization so we can ignore coverage for it.
    bool is_client {false};

    std::vector<SourceInfo> source_files;

    size_t instrumented_basic_blocks {0};
    std::vector<Addr> instrumented_blocks_addrs;
    std::vector<Line> instrumented_blocks_start_lines;

    String test_name;
    std::span<bool> current; /// Counters for currently active test.

    FileWrapper report_file;

    void writeReportHeader() noexcept;
    void symbolizeInstrumentedData();

    using SourceSymbolizedData = std::vector<std::pair<BBIndex, Line>>;
    using LocalCache = std::unordered_map<SourcePath, SourceSymbolizedData>;
    using LocalCaches = std::vector<LocalCache>;

    void symbolizeAddrsIntoLocalCaches(LocalCaches& caches);
    void mergeIntoGlobalCache(const LocalCaches& caches);
};
}
