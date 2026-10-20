#pragma once

#include "buffer_pool.hpp"
#include "renderer.hpp"
#include "renderer_stats.hpp"
#include <chrono>
#include <memory>

namespace orbelio {
class StaticGsRenderer;

// Owns the native target, pooled resources and the replaceable static GS backend.
class BootstrapRenderer final : public IVulkanRenderer {
public:
    explicit BootstrapRenderer(DeviceContext context, std::size_t gpuBudgetBytes = ModelLimits{}.gpuBytes);
    ~BootstrapRenderer() override;
    BootstrapRenderer(const BootstrapRenderer&) = delete;
    BootstrapRenderer& operator=(const BootstrapRenderer&) = delete;

    bool resize(std::uint32_t width, std::uint32_t height) override;
    void render(const FrameState& state) override;
    FrameReport finishFrame() override; // called AFTER Qt has submitted the frame
    void waitIdle() noexcept override;

    VkImage image() const override { return image_; }

    std::uint32_t width() const override { return width_; }

    std::uint32_t height() const override { return height_; }

private:
    // Construction: one step per owned resource. Each throws, the caller tears down.
    void createAllocator();
    void createCommandResources();
    void detectTimestampSupport();

    // Frame paths selected by render().
    void renderGaussians(const FrameState& state);
    void renderPlaceholder(const FrameState& state);
    void beginOneTimeCommands();
    void submitPlaceholderFrame();

    // Completion steps run by finishFrame(), in order.
    void submitCompletionFence();
    void readGpuTimestamps();
    void sampleHeapBudgets();

    void destroyTargetImage() noexcept;
    void destroy() noexcept;
    void waitSubmission();

    RenderThreadGuard guard_;
    DeviceContext context_;
    std::size_t gpuBudgetBytes_;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    std::unique_ptr<GpuBufferPool> buffers_;
    std::unique_ptr<StaticGsRenderer> gs_;

    VkImage image_ = VK_NULL_HANDLE;
    VkImageView imageView_ = VK_NULL_HANDLE;
    VmaAllocation imageAllocation_ = VK_NULL_HANDLE;

    VkCommandPool commands_ = VK_NULL_HANDLE;
    VkCommandBuffer command_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkQueryPool queries_ = VK_NULL_HANDLE;

    bool fencePending_ = false;
    bool imageHasContent_ = false;
    bool timestampsRecorded_ = false;

    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    double timestampPeriod_ = 0.0;
    std::uint32_t timestampBits_ = 0;

    FrameReport report_;
    std::chrono::steady_clock::time_point frameStart_;
    std::chrono::steady_clock::time_point previousCompletion_;
    bool hasPreviousCompletion_ = false;
};
} // namespace orbelio
