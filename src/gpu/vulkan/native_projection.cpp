#include "native_projection.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace orbelio {
namespace {
constexpr std::uint32_t projectGroupSize = 256;
// Rejected sentinels no longer extend the domain of positive FP32 camera depths.
constexpr std::uint32_t positiveDepthKeyBits = 31;
constexpr std::size_t projectionCounterWords = 12;
constexpr std::size_t projectionCounterBytes = projectionCounterWords * sizeof(std::uint32_t);
constexpr std::size_t projectedBytes = 12 * sizeof(float);
constexpr std::size_t normalBytes = 4 * sizeof(float);
constexpr VkBufferUsageFlags physicalBufferUsage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

struct alignas(16) ProjectionSettings {
    std::array<float, 4> intrinsics{};
    std::array<float, 4> clipping{};
};

static_assert(sizeof(ProjectionSettings) == 32);

std::uint64_t counter64(const std::array<std::uint32_t, projectionCounterWords>& words, std::size_t first) {
    return std::uint64_t(words[first]) | (std::uint64_t(words[first + 1]) << 32);
}

ProjectionDiagnostics decodeDiagnostics(const std::array<std::uint32_t, projectionCounterWords>& words) {
    ProjectionDiagnostics result{};
    result.clippedBoundingPixels = counter64(words, 1);
    result.maximumBoundingPixels = words[3];
    result.maximumEllipseArea = std::bit_cast<float>(words[4]);
    result.nearOpaquePeaks = words[7];
    result.maximumAxisRadius = std::bit_cast<float>(words[8]);
    result.axisRadiusOver512 = words[9];
    if (words[0]) {
        result.meanPeakOpacity = double(counter64(words, 5)) / 65535. / words[0];
        result.meanClippedEllipseAreaUpper = double(counter64(words, 10)) / 16. / words[0];
    }
    return result;
}

}

struct alignas(16) NativeProjection::ProjectParameters {
    std::array<float, 4> right{};
    std::array<float, 4> down{};
    std::array<float, 4> forward{};
    std::array<float, 4> eye{};
    std::array<float, 4> viewport{};
    std::array<std::uint32_t, 4> controls{};
    std::array<VkDeviceAddress, 4> addresses{};
};

NativeProjection::NativeProjection(DeviceContext context,
                                   VmaAllocator allocator,
                                   NativeBufferCounts& counts,
                                   std::string_view directory)
    : context_(context), allocator_(allocator), counts_(counts),
      sorter_(context, allocator, counts, directory, NativeSortMode::VisibleDepths),
      shaderDirectory_(directory) {
    try {
        createNativeCompute(
            context_, projection_, directory, "native_project.comp.spv", 4, sizeof(ProjectParameters));
        createNativeCompute(
            context_, shading_, directory, "native_shading.comp.spv", 1, sizeof(ProjectParameters));
    } catch (...) {
        destroy();
        throw;
    }
}

NativeProjection::~NativeProjection() { destroy(); }

void NativeProjection::destroy() noexcept {
    for (auto* resource : {&projectionSettings_,
                           &projected_,
                           &projectedNormals_,
                           &keys_,
                           &indices_,
                           &visibility_,
                           &counter_,
                           &counterReadback_}) {
        destroyBuffer(allocator_, *resource);
    }
    for (auto* resource : {&projection_, &projectionNormals_, &shading_}) {
        destroyPipeline(context_, *resource);
    }
}

void NativeProjection::reserve(std::size_t targetCount) {
    reserveBuffer(allocator_,
                  projectionSettings_,
                  sizeof(ProjectionSettings),
                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                  NativeHostAccess::None,
                  counts_);
    reserveBuffer(allocator_,
                  projected_,
                  targetCount * projectedBytes,
                  physicalBufferUsage,
                  NativeHostAccess::None,
                  counts_);
    reserveSortStorage(targetCount);
    reserveTelemetryStorage();
}

void NativeProjection::reserveSortStorage(std::size_t targetCount) {
    reserveBuffer(allocator_,
                  keys_,
                  targetCount * sizeof(std::uint32_t),
                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                  NativeHostAccess::None,
                  counts_);
    reserveBuffer(allocator_,
                  indices_,
                  targetCount * sizeof(std::uint32_t),
                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                  NativeHostAccess::None,
                  counts_);
    reserveBuffer(allocator_,
                  visibility_,
                  sizeof(NativeVisibilityControl),
                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                  NativeHostAccess::None,
                  counts_);
    sorter_.reserve(static_cast<std::uint32_t>(targetCount));
}

void NativeProjection::reserveTelemetryStorage() {
    reserveBuffer(allocator_,
                  counter_,
                  projectionCounterBytes,
                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                  NativeHostAccess::None,
                  counts_);
    reserveBuffer(allocator_,
                  counterReadback_,
                  projectionCounterBytes + sizeof(NativeVisibilityControl),
                  VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                  NativeHostAccess::Readback,
                  counts_);
}

NativeProjection::ProjectParameters NativeProjection::parameters(const FrameState& frame,
                                                                 const CameraView& camera,
                                                                 const NativeSceneUpload& model,
                                                                 std::uint32_t count,
                                                                 bool diagnostics) const {
    static_assert(sizeof(ProjectParameters) == 128);
    const auto& temporal = model.temporal();
    const auto& scene = model.scene();
    ProjectParameters result{};
    std::copy(camera.right.begin(), camera.right.end(), result.right.begin());
    std::copy(camera.down.begin(), camera.down.end(), result.down.begin());
    std::copy(camera.forward.begin(), camera.forward.end(), result.forward.begin());
    std::copy(camera.eye.begin(), camera.eye.end(), result.eye.begin());
    const auto& settings = frame.renderSettings;
    const bool scale = settings.attributeView == AttributeView::Scale;
    result.right[3] = scale ? std::log(settings.attributeMinimum) : settings.attributeMinimum;
    result.down[3] = scale ? std::log(settings.attributeMaximum) : settings.attributeMaximum;
    result.viewport = {float(frame.width), float(frame.height), camera.focalPixels, 0};
    if (temporal.active()) {
        result.forward[3] = float(temporal.time);
        result.eye[3] = float(temporal.time - double(result.forward[3]));
        result.viewport[3] = float(std::remainder(temporal.time, temporal.period) / temporal.period);
    }
    result.controls = {count,
                       scene.shDegree,
                       static_cast<std::uint32_t>(settings.attributeView),
                       std::uint32_t(scene.antialiased) | (std::uint32_t(diagnostics) << 1) |
                           (std::uint32_t(temporal.active()) << 2) | (std::uint32_t(diagnostics) << 3) |
                           (std::uint32_t(frame.renderSettings.separateShading) << 4) |
                           (temporal.degree << 8)};
    const auto addresses = model.addresses(context_);
    result.addresses = {addresses[0], addresses[1], addresses[2], bufferAddress(context_, projected_)};
    return result;
}

void NativeProjection::project(VkCommandBuffer command,
                               const FrameState& frame,
                               const CameraView& camera,
                               const NativeSceneUpload& model,
                               std::uint32_t count,
                               bool diagnostics) {
    const ProjectionSettings settings{camera.intrinsics,
                                      {camera.depthRange[0],
                                       camera.depthRange[1],
                                       float(camera.projection == CameraProjection::Orthographic),
                                       0}};
    vkCmdUpdateBuffer(command, projectionSettings_.handle, 0, sizeof(settings), &settings);
    if (diagnostics) {
        vkCmdFillBuffer(command, counter_.handle, 0, projectionCounterBytes, 0);
    }
    nativeMemoryBarrier(command,
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    const auto* normalBuffer = projectedNormals_.handle ? &projectedNormals_ : nullptr;
    const auto& pipeline = normalBuffer ? projectionNormals_ : projection_;
    bindNativeBuffers(command,
                      pipeline,
                      VK_PIPELINE_BIND_POINT_COMPUTE,
                      {&keys_, &indices_, &counter_, &projectionSettings_},
                      normalBuffer);
    const auto projectParameters = parameters(frame, camera, model, count, diagnostics);
    vkCmdPushConstants(command,
                       pipeline.layout,
                       VK_SHADER_STAGE_COMPUTE_BIT,
                       0,
                       sizeof(projectParameters),
                       &projectParameters);
    vkCmdDispatch(command, (count + projectGroupSize - 1) / projectGroupSize, 1, 1);
    nativeMemoryBarrier(command,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_ACCESS_SHADER_WRITE_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}

void NativeProjection::shade(VkCommandBuffer command,
                             const FrameState& frame,
                             const CameraView& camera,
                             const NativeSceneUpload& model,
                             std::uint32_t count,
                             bool diagnostics) {
    if (!frame.renderSettings.separateShading ||
        frame.renderSettings.attributeView != AttributeView::Normal) {
        return;
    }
    bindNativeBuffers(command, shading_, VK_PIPELINE_BIND_POINT_COMPUTE, {&projectionSettings_});
    const auto settings = parameters(frame, camera, model, count, diagnostics);
    vkCmdPushConstants(command, shading_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(settings), &settings);
    vkCmdDispatch(command, (count + projectGroupSize - 1) / projectGroupSize, 1, 1);
    nativeMemoryBarrier(command,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_ACCESS_SHADER_WRITE_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}

void NativeProjection::readTelemetry(BackendStats& stats, bool diagnostics) {
    vkCheck(vmaInvalidateAllocation(allocator_, counterReadback_.allocation, 0, counterReadback_.capacity),
            "vmaInvalidateAllocation native counters");
    NativeVisibilityControl compacted{};
    std::memcpy(&compacted,
                static_cast<const std::byte*>(counterReadback_.mapped) + projectionCounterBytes,
                sizeof(compacted));
    stats.visibleGaussians = compacted.sort.count;
    stats.sortedGaussians = compacted.sort.count;
    stats.projectionDiagnostics.reset();
    if (diagnostics) {
        std::array<std::uint32_t, projectionCounterWords> values{};
        std::memcpy(values.data(), counterReadback_.mapped, projectionCounterBytes);
        stats.independentlyProjectedGaussians = values[0];
        stats.projectionDiagnostics = decodeDiagnostics(values);
    }
}

NativeRasterInput NativeProjection::rasterInput(std::uint32_t count) const {
    return {projected_,
            keys_,
            indices_,
            count,
            &visibility_,
            projectedNormals_.handle ? &projectedNormals_ : nullptr};
}

void NativeProjection::reserveNormals(std::size_t targetCount, bool withNormals) {
    if (!withNormals) {
        destroyBuffer(allocator_, projectedNormals_);
        return;
    }
    reserveBuffer(allocator_,
                  projectedNormals_,
                  targetCount * normalBytes,
                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                  NativeHostAccess::None,
                  counts_);
    if (projectionNormals_.handle) {
        return;
    }
    try {
        createNativeCompute(context_,
                            projectionNormals_,
                            shaderDirectory_,
                            "native_project_normals.comp.spv",
                            5,
                            sizeof(ProjectParameters));
    } catch (...) {
        destroyPipeline(context_, projectionNormals_);
        throw;
    }
}

void NativeProjection::sort(VkCommandBuffer command, std::uint32_t count) {
    sorter_.record(command, count, keys_, indices_, &visibility_, positiveDepthKeyBits);
}

void NativeProjection::recordTelemetry(VkCommandBuffer command, bool diagnostics) {
    if (diagnostics) {
        const VkBufferCopy copy{0, 0, projectionCounterBytes};
        vkCmdCopyBuffer(command, counter_.handle, counterReadback_.handle, 1, &copy);
    }
    const VkBufferCopy copy{0, projectionCounterBytes, sizeof(NativeVisibilityControl)};
    vkCmdCopyBuffer(command, visibility_.handle, counterReadback_.handle, 1, &copy);
}

std::size_t NativeProjection::preprocessingBytes() const {
    return projected_.capacity + projectedNormals_.capacity + projectionSettings_.capacity;
}

std::size_t NativeProjection::sortingBytes() const {
    return keys_.capacity + indices_.capacity + visibility_.capacity + sorter_.bytes();
}

std::size_t NativeProjection::telemetryBytes() const { return counter_.capacity + counterReadback_.capacity; }
}
