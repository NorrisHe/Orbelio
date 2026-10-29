#pragma once
#include "renderer_stats.hpp"
#include "vma.hpp"
#include <memory>

namespace orbelio {
enum class GaussianRendererKind { Static, Fdu4d };

class IGaussianRenderer {
public:
    virtual ~IGaussianRenderer() = default;
    virtual void render(const FrameState& frame,
                        VkImage image,
                        VkImageView view,
                        bool initialized,
                        FrameReport& report) = 0;
};

class GaussianRendererFactory {
public:
    static GaussianRendererKind kind(const FrameState& frame);
    static std::unique_ptr<IGaussianRenderer> create(GaussianRendererKind kind,
                                                     DeviceContext context,
                                                     VmaAllocator allocator);
};
}
