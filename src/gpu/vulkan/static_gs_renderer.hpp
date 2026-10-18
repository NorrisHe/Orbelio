#pragma once
#include "renderer_stats.hpp"
#include "vma.hpp"
#include <orbelio/core/types.hpp>
#include <memory>

namespace orbelio {
// Temporary GPL backend adapter. Replacement boundary: no LFS types leave this header.
class StaticGsRenderer {
public:
    StaticGsRenderer(DeviceContext context, VmaAllocator allocator);
    ~StaticGsRenderer();
    void render(const FrameState& frame,
                VkImage image,
                VkImageView view,
                bool initialized,
                FrameReport& report);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
