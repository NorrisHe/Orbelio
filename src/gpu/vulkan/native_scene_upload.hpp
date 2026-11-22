#pragma once
#include "gaussian_temporal_view.hpp"
#include "native_gpu_resources.hpp"
#include <orbelio/core/types.hpp>

namespace orbelio {

class NativeSceneUpload {
public:
    enum class Change { None, Time, Upload };

    NativeSceneUpload(VmaAllocator allocator, NativeBufferCounts& counts);
    ~NativeSceneUpload();
    NativeSceneUpload(const NativeSceneUpload&) = delete;
    NativeSceneUpload& operator=(const NativeSceneUpload&) = delete;

    Change select(const FrameState& frame, const GaussianTemporalView& temporal);
    void reserveModel();
    std::size_t uploadEnd(const FrameState& frame) const;
    void reserveUpload(std::size_t end);
    void recordUpload(VkCommandBuffer command, std::size_t end);
    void commitUpload(std::size_t end);

    const StaticScene& scene() const { return *scene_; }

    const GaussianTemporalView& temporal() const { return temporal_; }

    std::size_t targetCount() const { return targetCount_; }

    std::size_t uploadedCount() const { return uploadedCount_; }

    std::uint64_t uploads() const { return uploads_; }

    std::array<VkDeviceAddress, 3> addresses(DeviceContext context) const;
    std::size_t bytes() const;

private:
    std::size_t positionStride() const;
    std::size_t harmonicStride() const;
    void packPosition(std::byte* output, std::size_t source) const;
    void packOrientation(std::byte* output, std::size_t source) const;
    void packHarmonics(std::byte* output, std::size_t source) const;
    void packUpload(std::size_t end);

    VmaAllocator allocator_;
    NativeBufferCounts& counts_;
    NativeBuffer positions_;
    NativeBuffer orientations_;
    NativeBuffer harmonics_;
    NativeBuffer staging_;
    std::shared_ptr<const StaticScene> scene_;
    GaussianTemporalView temporal_;
    std::size_t targetCount_ = 0;
    std::size_t uploadedCount_ = 0;
    std::uint64_t uploads_ = 0;
};
}
