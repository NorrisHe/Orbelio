#pragma once
#include "device_context.hpp"

namespace orbelio {
// Borrowed views. Submit consumers on DeviceContext::queue between render() and finishFrame().
// The next render(), resize() or renderer destruction may replace these images.
// Consumers restore the advertised layout; outputs support sampling and transfer-source reads.
struct VulkanRenderImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    VkExtent2D contentExtent{};
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct VulkanRenderImages {
    VulkanRenderImage display;
    VulkanRenderImage floatColorAlpha;
    VulkanRenderImage depth;
    VulkanRenderImage normals;
};
}
