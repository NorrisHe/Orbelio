#pragma once
#include "native_tile_raster.hpp"
#include <memory>

namespace orbelio {
bool nativeAllocationFailed(const VulkanError& error);

class NativeRasterizer {
public:
    NativeRasterizer(DeviceContext context,
                     VmaAllocator allocator,
                     NativeBufferCounts& counts,
                     const NativeRenderTargets& targets,
                     std::string_view directory);
    ~NativeRasterizer();
    NativeRasterizer(const NativeRasterizer&) = delete;
    NativeRasterizer& operator=(const NativeRasterizer&) = delete;

    void ensureNormals();
    bool prepare(const FrameState& frame, std::size_t targetCount, std::size_t normalStorageBytes);

    bool tiled() const { return bool(tiler_); }

    void clearRejection() { rejectedTile_.reset(); }

    void releaseTiles() { tiler_.reset(); }

    void recordBinning(VkCommandBuffer command,
                       const FrameState& frame,
                       NativeRasterInput input,
                       NativeTileQueries queries);
    void record(VkCommandBuffer command,
                const FrameState& frame,
                NativeRasterInput input,
                VkQueryPool statisticsQueries,
                bool diagnostics);
    void recordTelemetry(VkCommandBuffer command);
    void readTelemetry(bool diagnostics);
    void fillStats(const FrameState& frame, BackendStats& stats) const;
    std::size_t sortingBytes() const;
    std::size_t telemetryBytes() const;

private:
    void createGraphicsPipeline(NativePipeline& pipeline, bool withDepth, bool withNormals);
    void createGraphicsStages(VkShaderModule vertex,
                              VkShaderModule fragment,
                              NativePipeline& pipeline,
                              bool withDepth,
                              bool withNormals);
    void recordQuads(VkCommandBuffer command, VkExtent2D extent, NativeRasterInput input);
    void destroy() noexcept;

    DeviceContext context_;
    VmaAllocator allocator_;
    NativeBufferCounts& counts_;
    const NativeRenderTargets& targets_;
    std::string shaderDirectory_;
    std::unique_ptr<NativeTileRaster> tiler_;
    std::optional<std::array<std::size_t, 5>> rejectedTile_;
    NativePipeline raster_;
    NativePipeline rasterDepth_;
    NativePipeline rasterNormals_;
    NativePipeline rasterDepthNormals_;
};
}
