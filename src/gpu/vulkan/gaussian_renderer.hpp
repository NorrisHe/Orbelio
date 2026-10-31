#pragma once
#include "renderer_stats.hpp"
#include "device_context.hpp"

namespace orbelio {
class IGaussianRenderer {
public:
    virtual ~IGaussianRenderer() = default;
    virtual void render(const FrameState& frame,
                        VkImage image,
                        VkImageView view,
                        bool initialized,
                        FrameReport& report) = 0;
};

}
