#include "native_scene_upload.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace orbelio {
namespace {
constexpr std::size_t positionBytes = 4 * sizeof(float);
constexpr std::size_t orientationBytes = 8 * sizeof(float);
constexpr VkBufferUsageFlags physicalBufferUsage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
constexpr VkBufferUsageFlags modelBufferUsage = physicalBufferUsage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

void validateScene(const StaticScene& scene) {
    const auto count = scene.count();
    if (scene.shDegree > 3 || count > std::numeric_limits<std::uint32_t>::max() ||
        scene.positions.size() != count * 3 || scene.rotations.size() != count * 4 ||
        scene.logScales.size() != count * 3 || scene.sh0.size() != count * 3 ||
        scene.shRest.size() != count * scene.shRestCoefficients() * 3) {
        throw std::invalid_argument("Invalid Gaussian scene layout");
    }
}

}

NativeSceneUpload::NativeSceneUpload(VmaAllocator allocator, NativeBufferCounts& counts)
    : allocator_(allocator), counts_(counts) {}

NativeSceneUpload::~NativeSceneUpload() {
    for (auto* resource : {&positions_, &orientations_, &harmonics_, &staging_}) {
        destroyBuffer(allocator_, *resource);
    }
}

std::size_t NativeSceneUpload::positionStride() const { return positionBytes * (temporal_.active() ? 2 : 1); }

std::size_t NativeSceneUpload::harmonicStride() const {
    const auto coefficients = (scene_->shRestCoefficients() + 1) * 3;
    return (coefficients * (temporal_.degree + 1) + std::size_t(temporal_.active())) * sizeof(float);
}

NativeSceneUpload::Change NativeSceneUpload::select(const FrameState& frame,
                                                    const GaussianTemporalView& nextTemporal) {
    if (!frame.scene) {
        throw std::invalid_argument("Missing Gaussian scene");
    }
    if (scene_ != frame.scene) {
        validateScene(*frame.scene);
    }
    const auto nextCount = frame.renderSettings.budgetedCount(frame.scene->count());
    const bool changed = scene_ != frame.scene || targetCount_ != nextCount ||
                         temporal_.active() != nextTemporal.active() ||
                         temporal_.degree != nextTemporal.degree || temporal_.period != nextTemporal.period ||
                         temporal_.times.data() != nextTemporal.times.data() ||
                         temporal_.variances.data() != nextTemporal.variances.data() ||
                         temporal_.velocities.data() != nextTemporal.velocities.data() ||
                         temporal_.harmonics.data() != nextTemporal.harmonics.data();
    const bool timeChanged = temporal_.time != nextTemporal.time;
    if (changed) {
        scene_ = frame.scene;
        targetCount_ = nextCount;
        uploadedCount_ = 0;
        ++uploads_;
    }
    temporal_ = nextTemporal;
    if (changed) {
        return Change::Upload;
    }
    return timeChanged ? Change::Time : Change::None;
}

void NativeSceneUpload::packPosition(std::byte* output, std::size_t source) const {
    const std::array<float, 4> position{scene_->positions[source * 3],
                                        scene_->positions[source * 3 + 1],
                                        scene_->positions[source * 3 + 2],
                                        scene_->opacityLogits[source]};
    std::memcpy(output, position.data(), positionBytes);
    if (temporal_.active()) {
        const std::array<float, 4> motion{temporal_.velocities[source * 3],
                                          temporal_.velocities[source * 3 + 1],
                                          temporal_.velocities[source * 3 + 2],
                                          temporal_.times[source]};
        std::memcpy(output + positionBytes, motion.data(), positionBytes);
    }
}

void NativeSceneUpload::packOrientation(std::byte* output, std::size_t source) const {
    std::memcpy(output, scene_->rotations.data() + source * 4, 4 * sizeof(float));
    const std::array<float, 4> scale{scene_->logScales[source * 3],
                                     scene_->logScales[source * 3 + 1],
                                     scene_->logScales[source * 3 + 2],
                                     temporal_.active() ? temporal_.variances[source] : 0};
    std::memcpy(output + 4 * sizeof(float), scale.data(), 4 * sizeof(float));
}

void NativeSceneUpload::packHarmonics(std::byte* output, std::size_t source) const {
    const auto baseBytes = (scene_->shRestCoefficients() + 1) * 3 * sizeof(float);
    std::memcpy(output, scene_->sh0.data() + source * 3, 3 * sizeof(float));
    if (scene_->shRestCoefficients()) {
        std::memcpy(output + 3 * sizeof(float),
                    scene_->shRest.data() + source * scene_->shRestCoefficients() * 3,
                    baseBytes - 3 * sizeof(float));
    }
    if (temporal_.active()) {
        const auto temporalBytes = baseBytes * temporal_.degree;
        if (temporalBytes) {
            std::memcpy(output + baseBytes,
                        temporal_.harmonics.data() + source * temporalBytes / sizeof(float),
                        temporalBytes);
        }
        const auto phase =
            float(std::remainder(double(temporal_.times[source]), temporal_.period) / temporal_.period);
        std::memcpy(output + baseBytes + temporalBytes, &phase, sizeof(phase));
    }
}

void NativeSceneUpload::packUpload(std::size_t end) {
    const auto points = end - uploadedCount_;
    auto* bytes = static_cast<std::byte*>(staging_.mapped);
    for (std::size_t i = 0; i < points; ++i) {
        const auto source = sampledGaussian(uploadedCount_ + i, scene_->count(), targetCount_);
        packPosition(bytes + i * positionStride(), source);
        packOrientation(bytes + points * positionStride() + i * orientationBytes, source);
        packHarmonics(bytes + points * (positionStride() + orientationBytes) + i * harmonicStride(), source);
    }
}

std::size_t NativeSceneUpload::uploadEnd(const FrameState& frame) const {
    const auto batch = frame.renderSettings.uploadGaussiansPerFrame;
    return batch ? std::min(targetCount_, uploadedCount_ + batch) : targetCount_;
}

void NativeSceneUpload::reserveUpload(std::size_t end) {
    const auto points = end - uploadedCount_;
    if (!points) {
        return;
    }
    const auto bytes = points * (positionStride() + orientationBytes + harmonicStride());
    reserveBuffer(
        allocator_, staging_, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, NativeHostAccess::Upload, counts_);
}

void NativeSceneUpload::recordUpload(VkCommandBuffer command, std::size_t end) {
    if (end == uploadedCount_) {
        return;
    }
    const auto points = end - uploadedCount_;
    const auto bytes = points * (positionStride() + orientationBytes + harmonicStride());
    packUpload(end);
    vkCheck(vmaFlushAllocation(allocator_, staging_.allocation, 0, bytes),
            "vmaFlushAllocation native upload");
    const std::array<const NativeBuffer*, 3> targets{&positions_, &orientations_, &harmonics_};
    const std::array strides{positionStride(), orientationBytes, harmonicStride()};
    std::size_t offset = 0;
    for (std::size_t i = 0; i < targets.size(); ++i) {
        const VkBufferCopy copy{offset, uploadedCount_ * strides[i], points * strides[i]};
        vkCmdCopyBuffer(command, staging_.handle, targets[i]->handle, 1, &copy);
        offset += points * strides[i];
    }
}

void NativeSceneUpload::reserveModel() {
    reserveBuffer(allocator_,
                  positions_,
                  targetCount_ * positionStride(),
                  modelBufferUsage,
                  NativeHostAccess::None,
                  counts_);
    reserveBuffer(allocator_,
                  orientations_,
                  targetCount_ * orientationBytes,
                  modelBufferUsage,
                  NativeHostAccess::None,
                  counts_);
    reserveBuffer(allocator_,
                  harmonics_,
                  targetCount_ * harmonicStride(),
                  modelBufferUsage,
                  NativeHostAccess::None,
                  counts_);
}

void NativeSceneUpload::commitUpload(std::size_t end) {
    if (temporal_.active() && uploadedCount_ != end && end == targetCount_) {
        destroyBuffer(allocator_, staging_);
    }
    uploadedCount_ = end;
}

std::array<VkDeviceAddress, 3> NativeSceneUpload::addresses(DeviceContext context) const {
    return {bufferAddress(context, positions_),
            bufferAddress(context, orientations_),
            bufferAddress(context, harmonics_)};
}

std::size_t NativeSceneUpload::bytes() const {
    return positions_.capacity + orientations_.capacity + harmonics_.capacity + staging_.capacity;
}
}
