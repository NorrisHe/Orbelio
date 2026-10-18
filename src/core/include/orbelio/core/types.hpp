#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <memory>
#include "static_scene.hpp"
#include <vector>

namespace orbelio {

struct FrameCapture {
    std::uint64_t requestId = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
};

struct FrameState {
    std::shared_ptr<const StaticScene> scene;
    std::uint64_t captureRequestId = 0; // Zero keeps viewport pixels on the GPU.
    double timestampSeconds = 0.0;
    std::uint32_t width = 1;
    std::uint32_t height = 1;
};

struct PoolStats {
    std::size_t activeBytes = 0;
    std::size_t reservedBytes = 0;
    std::size_t peakBytes = 0;
    std::uint64_t allocations = 0;
    std::uint64_t reuses = 0;
};

// What any renderer must report about one frame. Implementation-specific counters live with the
// backend (src/gpu/vulkan/renderer_stats.hpp) so swapping it never touches core.
struct FrameStats {
    std::uint64_t gaussianCount = 0;
    std::uint64_t sceneUploads = 0;
    double orbitDegrees = 0;
    std::shared_ptr<const FrameCapture> capture;

    std::optional<double> projectionGpuMs;
    std::optional<double> sortingGpuMs;
    std::optional<double> rasterGpuMs;
    std::optional<double> composeGpuMs;
    std::optional<double> gpuRenderMs;
    std::optional<double> frameIntervalMs;
    double completeFrameMs = 0;
    double cpuSubmitMs = 0.0;
    double gpuWaitMs = 0.0;
    double backendWaitMs = 0;
    double queryReadbackCpuMs = 0;

    std::uint32_t viewportWidth = 0;
    std::uint32_t viewportHeight = 0;
    std::uint64_t frameIndex = 0;
};

} // namespace orbelio
