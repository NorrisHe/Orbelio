#include "renderer.hpp"
#include "bootstrap_renderer.hpp"

namespace orbelio {
VulkanRendererFactory::VulkanRendererFactory(DeviceContext context, std::size_t gpuBudgetBytes)
    : context_(context), gpuBudgetBytes_(gpuBudgetBytes) {}

std::unique_ptr<IVulkanRenderer> VulkanRendererFactory::create() const {
    return std::make_unique<BootstrapRenderer>(context_, gpuBudgetBytes_);
}
}
