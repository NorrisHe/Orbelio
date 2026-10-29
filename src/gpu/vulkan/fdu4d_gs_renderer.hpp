#pragma once
#include "static_gs_renderer.hpp"

namespace orbelio {
class Fdu4dGsRenderer final : public IGaussianRenderer {
public:
    Fdu4dGsRenderer(DeviceContext context, VmaAllocator allocator) : rasterizer_(context, allocator) {}

    void render(const FrameState& frame,
                VkImage image,
                VkImageView view,
                bool initialized,
                FrameReport& report) override;

private:
    StaticGsRenderer rasterizer_;
    std::shared_ptr<const Fdu4dScene> source_;
    std::shared_ptr<const StaticScene> evaluated_;
    double time_ = 0;
    std::size_t count_ = 0;
};
}
