#pragma once
#include "renderer_stats.hpp"
#include "vma.hpp"
#include "gaussian_renderer.hpp"
#include <orbelio/core/types.hpp>
#include <memory>

namespace orbelio {
// Native EWA projection, stable GPU sorting and ordered floating-point alpha compositing.
class StaticGsRenderer final : public IGaussianRenderer {
public:
    StaticGsRenderer(DeviceContext context, VmaAllocator allocator, std::string shaderDirectory = {});
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
