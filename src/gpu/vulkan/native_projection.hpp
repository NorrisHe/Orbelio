#pragma once
#include "native_radix_sort.hpp"
#include "native_raster_input.hpp"
#include "renderer_stats.hpp"
#include "native_scene_upload.hpp"
#include <string>

namespace orbelio {

class NativeProjection {
public:
    NativeProjection(DeviceContext context,
                     VmaAllocator allocator,
                     NativeBufferCounts& counts,
                     std::string_view directory);
    ~NativeProjection();
    NativeProjection(const NativeProjection&) = delete;
    NativeProjection& operator=(const NativeProjection&) = delete;

    void reserve(std::size_t targetCount);
    void reserveNormals(std::size_t targetCount, bool withNormals);
    void project(VkCommandBuffer command,
                 const FrameState& frame,
                 const CameraView& camera,
                 const NativeSceneUpload& model,
                 std::uint32_t count,
                 bool diagnostics);
    void shade(VkCommandBuffer command,
               const FrameState& frame,
               const CameraView& camera,
               const NativeSceneUpload& model,
               std::uint32_t count,
               bool diagnostics);
    void sort(VkCommandBuffer command, std::uint32_t count);
    NativeRasterInput rasterInput(std::uint32_t count) const;
    void recordTelemetry(VkCommandBuffer command, bool diagnostics);
    void readTelemetry(BackendStats& stats, bool diagnostics);
    std::size_t preprocessingBytes() const;
    std::size_t sortingBytes() const;
    std::size_t telemetryBytes() const;

    std::size_t normalStorageBytes() const { return projectedNormals_.capacity; }

private:
    struct ProjectParameters;
    ProjectParameters parameters(const FrameState& frame,
                                 const CameraView& camera,
                                 const NativeSceneUpload& model,
                                 std::uint32_t count,
                                 bool diagnostics) const;
    void reserveSortStorage(std::size_t targetCount);
    void reserveTelemetryStorage();
    void destroy() noexcept;

    DeviceContext context_;
    VmaAllocator allocator_;
    NativeBufferCounts& counts_;
    NativeRadixSort sorter_;
    std::string shaderDirectory_;
    NativePipeline projection_;
    NativePipeline projectionNormals_;
    NativePipeline shading_;
    NativeBuffer projectionSettings_;
    NativeBuffer projected_;
    NativeBuffer projectedNormals_;
    NativeBuffer keys_;
    NativeBuffer indices_;
    NativeBuffer visibility_;
    NativeBuffer counter_;
    NativeBuffer counterReadback_;
};
}
