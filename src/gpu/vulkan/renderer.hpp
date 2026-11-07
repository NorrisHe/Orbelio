#pragma once

#include "device_context.hpp"
#include "renderer_stats.hpp"
#include "render_images.hpp"
#include <orbelio/core/types.hpp>
#include <memory>

namespace orbelio {
class IVulkanRenderer {
public:
    virtual ~IVulkanRenderer() = default;
    virtual bool resize(std::uint32_t width, std::uint32_t height) = 0;
    virtual void render(const FrameState& state) = 0;
    virtual FrameReport finishFrame() = 0;
    virtual void waitIdle() noexcept = 0;
    virtual VkImage image() const = 0;
    virtual VulkanRenderImages outputImages() const = 0;
    virtual std::uint32_t width() const = 0;
    virtual std::uint32_t height() const = 0;
};

class IVulkanRendererFactory {
public:
    virtual ~IVulkanRendererFactory() = default;
    // Creation and destruction belong to the consuming render thread.
    virtual std::unique_ptr<IVulkanRenderer> create() const = 0;
};

class VulkanRendererFactory final : public IVulkanRendererFactory {
public:
    explicit VulkanRendererFactory(DeviceContext context,
                                   std::size_t gpuBudgetBytes = ModelLimits{}.gpuBytes,
                                   std::string shaderDirectory = {});
    std::unique_ptr<IVulkanRenderer> create() const override;

private:
    DeviceContext context_;
    std::size_t gpuBudgetBytes_;
    std::string shaderDirectory_;
};
}
