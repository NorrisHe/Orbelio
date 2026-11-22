#pragma once
#include "gaussian_temporal_view.hpp"
#include "vma.hpp"
#include "gaussian_renderer.hpp"
#include <orbelio/core/types.hpp>
#include <memory>

namespace orbelio {
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
