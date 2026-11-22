#pragma once
#include "native_radix_sort.hpp"
#include "native_raster_input.hpp"
#include "native_render_targets.hpp"

namespace orbelio {
class NativeTileRaster {
public:
    NativeTileRaster(DeviceContext context,
                     VmaAllocator allocator,
                     NativeBufferCounts& counts,
                     std::string_view directory);
    ~NativeTileRaster();
    NativeTileRaster(const NativeTileRaster&) = delete;
    NativeTileRaster& operator=(const NativeTileRaster&) = delete;
    void reserve(const FrameState& frame, std::size_t maximumCount, bool withNormals = false);
    void recordBinning(VkCommandBuffer command,
                       const FrameState& frame,
                       NativeRasterInput input,
                       NativeTileQueries queries = {});
    void recordRaster(VkCommandBuffer command,
                      const FrameState& frame,
                      NativeRasterInput input,
                      const NativeRenderTargets& targets,
                      bool diagnostics);
    void recordTelemetry(VkCommandBuffer command);
    void readTelemetry(bool diagnostics);
    void fillStats(BackendStats& stats) const;
    std::size_t workspaceBytes() const;

    std::size_t telemetryBytes() const { return readback_.capacity; }

    void recordFallbackDraw(VkCommandBuffer command) const;

private:
    struct Parameters;

    struct ScanBuffers {
        const NativeBuffer& input;
        const NativeBuffer& offsets;
        const NativeBuffer& totals;
    };

    void reserveScan(std::size_t maximumCount);
    void recordCounts(VkCommandBuffer command, NativeRasterInput input, const Parameters& settings);
    void recordEmission(VkCommandBuffer command, const Parameters& settings);
    void reserveTelemetry(std::size_t maximumCount);
    void recordScan(VkCommandBuffer command,
                    Parameters settings,
                    ScanBuffers buffers,
                    std::uint32_t count,
                    std::uint32_t level);
    Parameters parameters(const FrameState& frame, NativeRasterInput input, bool depth = false) const;
    void createPipelines(std::string_view directory);
    void createRasterPipelines(std::string_view directory, bool withNormals);
    void bindRaster(VkCommandBuffer command,
                    NativeRasterInput input,
                    const NativeRenderTargets& targets,
                    const NativePipeline& pipeline,
                    bool diagnostics);
    void transitionTargets(VkCommandBuffer command, const NativeRenderTargets& targets, bool writing);
    void destroy() noexcept;

    DeviceContext context_;
    VmaAllocator allocator_;
    NativeBufferCounts& counts_;
    NativeRadixSort sorter_;
    std::string shaderDirectory_;
    NativePipeline sourceDispatch_;
    NativePipeline bin_;
    NativePipeline dispatch_;
    NativePipeline cooperativeDispatch_;
    NativePipeline cooperativeCount_;
    NativePipeline cooperativeEmit_;
    NativePipeline scan_;
    NativePipeline emit_;
    NativePipeline rangesPipeline_;
    std::array<std::array<NativePipeline, 4>, 2> rasterPipelines_{};
    NativeBuffer pairCounts_;
    NativeBuffer bounds_;
    NativeBuffer offsets_;
    NativeBuffer groupSums_;
    NativeBuffer groupOffsets_;
    NativeBuffer chunkSums_;
    NativeBuffer chunkOffsets_;
    NativeBuffer total_;
    NativeBuffer orders_;
    NativeBuffer tiles_;
    NativeBuffer ranges_;
    NativeBuffer control_;
    NativeBuffer readback_;
    std::uint32_t capacity_ = 0;
    std::optional<std::uint32_t> reserved_;
    std::optional<std::uint32_t> demand_;
    std::optional<std::uint32_t> cooperative_;
    std::optional<bool> overflow_;
    std::optional<bool> selected_;
    std::optional<TileDiagnostics> diagnostics_;
};
}
