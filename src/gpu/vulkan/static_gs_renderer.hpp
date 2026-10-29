#pragma once
#include "renderer_stats.hpp"
#include "vma.hpp"
#include "gaussian_renderer.hpp"
#include <orbelio/core/types.hpp>
#include <memory>

namespace orbelio {
// Temporary GPL backend adapter. Replacement boundary: no LFS types leave this header.
class StaticGsRenderer final : public IGaussianRenderer {
public:
    StaticGsRenderer(DeviceContext context, VmaAllocator allocator);
    ~StaticGsRenderer() override;
    void render(const FrameState& frame,
                VkImage image,
                VkImageView view,
                bool initialized,
                FrameReport& report) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
