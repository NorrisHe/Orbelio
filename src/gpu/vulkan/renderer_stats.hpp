#pragma once

#include <orbelio/core/types.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace orbelio {
struct ProjectionDiagnostics {
    std::uint64_t clippedBoundingPixels = 0;
    std::uint32_t maximumBoundingPixels = 0;
    double meanClippedEllipseAreaUpper = 0;
    double maximumEllipseArea = 0;
    double meanPeakOpacity = 0;
    std::uint32_t nearOpaquePeaks = 0;
    double maximumAxisRadius = 0;
    std::uint32_t axisRadiusOver512 = 0;
};

struct TileStageTimings {
    double countingMs = 0;
    double prefixMs = 0;
    double emissionMs = 0;
    double sortingMs = 0;
    double rangesMs = 0;
};

struct TileDiagnostics {
    std::uint64_t testedPixelGaussians = 0;
    std::uint64_t contributingPixelGaussians = 0;
    std::uint64_t testedBelowTransmittance1e4 = 0;
    std::uint32_t zeroTransmittancePixels = 0;
    std::uint32_t nearOpaquePixels = 0;
};

// Resource and pipeline counters that only this backend can produce. A replacement backend defines
// its own type here; core and the UI keep reading FrameStats unchanged.
struct BackendStats {
    std::string rasterizer;
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
    std::uint64_t imageAllocations = 0;
    std::size_t imageReservedBytes = 0;

    std::optional<std::uint64_t> visibleGaussians;
    std::optional<std::uint64_t> sortedGaussians;
    std::optional<std::uint64_t> independentlyProjectedGaussians;
    std::optional<std::uint32_t> tilePairsReserved;
    std::optional<std::uint32_t> cooperativeGaussians;
    std::optional<std::uint32_t> tilePairsDemandLowerBound;
    std::uint32_t tilePairCapacity = 0;
    std::uint32_t tileBinningWidth = 0;
    std::optional<bool> tilePairOverflow;
    std::optional<bool> tiledRasterization;
    bool tileAllocationFallback = false;
    std::optional<ProjectionDiagnostics> projectionDiagnostics;
    std::optional<std::uint64_t> fragmentShaderInvocations;
    std::optional<TileDiagnostics> tileDiagnostics;
    std::optional<TileStageTimings> tileStageTimings;
    std::uint64_t telemetryFrameIndex = 0;
    double telemetryAgeMs = 0;
};

struct FrameReport {
    FrameStats frame;
    BackendStats backend;
};
} // namespace orbelio
