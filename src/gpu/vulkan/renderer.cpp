#include "renderer.hpp"
#include "bootstrap_renderer.hpp"

namespace orbelio {
VulkanRendererFactory::VulkanRendererFactory(DeviceContext context) : context_(context) {}

std::unique_ptr<IVulkanRenderer> VulkanRendererFactory::create() const {
    return std::make_unique<BootstrapRenderer>(context_);
}
}
