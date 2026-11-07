#pragma once
#include "renderer_stats.hpp"
#include "render_images.hpp"
#include "vma.hpp"
#include <array>

namespace orbelio {
struct NativeImage {
    VulkanRenderImage resource;
    VmaAllocation memory = VK_NULL_HANDLE;
    std::size_t bytes = 0;
    bool hasContent = false;
};

class NativeRenderTargets {
public:
    NativeRenderTargets(DeviceContext context, VmaAllocator allocator);
    ~NativeRenderTargets();
    NativeRenderTargets(const NativeRenderTargets&) = delete;
    NativeRenderTargets& operator=(const NativeRenderTargets&) = delete;
    void ensure(const FrameState& frame, RenderOutput outputs, FrameReport& report);

    VkRenderPass pass(bool withDepth, bool withNormals) const {
        return passes_[std::size_t(withDepth) + 2 * std::size_t(withNormals)];
    }

    VkFramebuffer framebuffer() const { return framebuffer_; }

    bool hasDepth() const { return depthAccum.resource.image != VK_NULL_HANDLE; }

    bool hasNormals() const { return normals.resource.image != VK_NULL_HANDLE; }

    std::size_t bytes() const;
    VulkanRenderImages outputImages() const;

    NativeImage colorAccum;
    NativeImage depthAccum;
    NativeImage colorAlpha;
    NativeImage depth;
    NativeImage normals;

private:
    void createPass(bool withDepth, bool withNormals);
    void createImage(NativeImage& image, VkExtent2D extent, VkImageUsageFlags usage, FrameReport& report);
    void createFramebuffer(VkExtent2D extent);
    void destroyImage(NativeImage& image) noexcept;
    void destroyFramebuffer() noexcept;
    void destroyImages() noexcept;
    void destroy() noexcept;

    DeviceContext context_;
    VmaAllocator allocator_;
    std::array<VkRenderPass, 4> passes_{};
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
};
}
