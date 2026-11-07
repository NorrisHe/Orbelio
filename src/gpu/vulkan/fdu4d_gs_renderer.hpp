#pragma once
#include "native_gaussian_renderer.hpp"

namespace orbelio {
class Fdu4dGsRenderer final : public IGaussianRenderer {
public:
    Fdu4dGsRenderer(DeviceContext context, VmaAllocator allocator, std::string shaderDirectory = {})
        : rasterizer_(context, allocator, std::move(shaderDirectory)) {}

    VulkanRenderImages outputImages() const override { return rasterizer_.outputImages(); }

    void render(const FrameState& frame,
                VkImage image,
                VkImageView view,
                bool initialized,
                FrameReport& report) override;

private:
    NativeGaussianRenderer rasterizer_;
};
}
