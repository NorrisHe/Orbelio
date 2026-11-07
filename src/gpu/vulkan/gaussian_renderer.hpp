#pragma once
#include "renderer_stats.hpp"
#include "device_context.hpp"
#include "render_images.hpp"

namespace orbelio {
class IGaussianRenderer {
public:
    virtual ~IGaussianRenderer() = default;
    virtual VulkanRenderImages outputImages() const = 0;
    virtual void render(const FrameState& frame,
                        VkImage image,
                        VkImageView view,
                        bool initialized,
                        FrameReport& report) = 0;
};

}
