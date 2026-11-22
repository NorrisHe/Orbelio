#pragma once
#include "native_capture.hpp"
#include "native_gpu_resources.hpp"
#include "native_render_targets.hpp"

namespace orbelio {
struct NativeDisplayTarget {
    VkImage image;
    VkImageView view;
    bool initialized;
};

class NativeFrameOutput {
public:
    NativeFrameOutput(DeviceContext context,
                      VmaAllocator allocator,
                      NativeBufferCounts& counts,
                      std::string_view directory);
    ~NativeFrameOutput();
    NativeFrameOutput(const NativeFrameOutput&) = delete;
    NativeFrameOutput& operator=(const NativeFrameOutput&) = delete;

    const NativeRenderTargets& targets() const { return targets_; }

    void reserveTargets(const FrameState& frame, bool capture, FrameReport& report);
    void reserveCapture(const FrameState& frame, const NativeCapturePlan& plan);
    bool capturePending(const FrameState& frame) const;

    void invalidateCapture() { capture_.reset(); }

    void composeImage(VkCommandBuffer command, const FrameState& frame, NativeDisplayTarget display);
    void recordCapture(VkCommandBuffer command,
                       const FrameState& frame,
                       VkImage display,
                       const NativeCapturePlan& plan);
    void publishLayouts(VkCommandBuffer command, VkImage display, RenderOutput captured);
    void completeSubmission();
    void readCapture(const FrameState& frame, const NativeCapturePlan& plan);
    void fillCaptureStats(FrameStats& stats) const;

    std::size_t telemetryBytes() const { return pixelReadback_.capacity; }

private:
    void createSampler();
    void createComposePipeline(std::string_view directory, NativePipeline& pipeline, bool withFloat);
    void bindComposeImages(VkCommandBuffer command,
                           const NativePipeline& pipeline,
                           VkImageView display,
                           bool withFloat);
    VkImage captureImage(RenderOutput output, VkImage display) const;
    void destroy() noexcept;

    DeviceContext context_;
    VmaAllocator allocator_;
    NativeBufferCounts& counts_;
    NativeRenderTargets targets_;
    VkSampler sampler_ = VK_NULL_HANDLE;
    NativePipeline compose_;
    NativePipeline composeFloat_;
    NativeBuffer pixelReadback_;
    std::shared_ptr<const FrameCapture> capture_;
    std::uint64_t pixelReadbacks_ = 0;
};
}
