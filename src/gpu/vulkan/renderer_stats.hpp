#pragma once

#include <orbelio/core/types.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace orbelio {
// Resource and pipeline counters that only this backend can produce. A replacement backend defines
// its own type here; core and the UI keep reading FrameStats unchanged.
struct BackendStats {
    std::uint64_t tileInstances = 0;
    std::uint64_t depthWaves = 0;
    std::uint64_t gsBufferAllocations = 0;
    std::uint64_t gsBufferReuses = 0;
    std::uint64_t gsGrowths = 0;
    std::size_t gsReservedBytes = 0;

    std::size_t modelBytes = 0;
    std::size_t preprocessingBytes = 0;
    std::size_t sortingBytes = 0;
    std::size_t pixelBytes = 0;
    std::size_t telemetryBytes = 0;
    std::size_t allocatorUsedBytes = 0;
    std::size_t allocatorReservedBytes = 0;
    std::size_t allocatorPeakBytes = 0;
    std::size_t allocatorBudgetBytes = 0;
    PoolStats buffers;
    std::uint64_t imageAllocations = 0;
    std::size_t imageReservedBytes = 0;

    std::optional<std::uint64_t> visibleGaussians;
    std::optional<std::uint64_t> maxTileList;
    std::optional<std::uint64_t> telemetryReferences;
    std::uint64_t telemetryFrameIndex = 0;
    double telemetryAgeMs = 0;
    std::string telemetryError;
};

struct FrameReport {
    FrameStats frame;
    BackendStats backend;
};
} // namespace orbelio
