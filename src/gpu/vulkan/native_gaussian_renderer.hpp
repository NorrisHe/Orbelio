#pragma once
#include "renderer_stats.hpp"
#include "vma.hpp"
#include "gaussian_renderer.hpp"
#include <orbelio/core/types.hpp>
#include <memory>
#include <span>

namespace orbelio {
// Borrowed spans stay alive through the immutable owner in FrameState::scene.
struct GaussianTemporalView {
    std::span<const float> times;
    std::span<const float> variances;
    std::span<const float> velocities;
    std::span<const float> harmonics;
    unsigned degree = 0;
    double time = 0;
    double period = 1;

    bool active() const { return !times.empty(); }
};

// Native EWA projection, stable GPU sorting and ordered floating-point alpha compositing.
class NativeGaussianRenderer final : public IGaussianRenderer {
public:
    NativeGaussianRenderer(DeviceContext context, VmaAllocator allocator, std::string shaderDirectory = {});
    ~NativeGaussianRenderer() override;
    VulkanRenderImages outputImages() const override;
    void render(const FrameState& frame,
                VkImage image,
                VkImageView view,
                bool initialized,
                FrameReport& report) override;
    void renderTemporal(const FrameState& frame,
                        const GaussianTemporalView& temporal,
                        VkImage image,
                        VkImageView view,
                        bool initialized,
                        FrameReport& report);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
