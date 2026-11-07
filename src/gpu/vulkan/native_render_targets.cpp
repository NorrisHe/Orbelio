#include "native_render_targets.hpp"
#include <array>

namespace orbelio {
namespace {
constexpr VkFormat floatFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
constexpr auto accumulationUsage =
    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
constexpr auto outputUsage =
    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
}

NativeRenderTargets::NativeRenderTargets(DeviceContext context, VmaAllocator allocator)
    : context_(context), allocator_(allocator) {
    try {
        VkFormatProperties supported{};
        vkGetPhysicalDeviceFormatProperties(context_.physicalDevice, floatFormat, &supported);
        const auto required = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
                              VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if ((supported.optimalTilingFeatures & required) != required) {
            throw std::runtime_error("Native rendering requires RGBA32F blending, sampling and storage");
        }
        for (const bool normals : {false, true}) {
            for (const bool depth : {false, true}) {
                createPass(depth, normals);
            }
        }
    } catch (...) {
        destroy();
        throw;
    }
}

NativeRenderTargets::~NativeRenderTargets() { destroy(); }

void NativeRenderTargets::destroyFramebuffer() noexcept {
    vkDestroyFramebuffer(context_.device, framebuffer_, nullptr);
    framebuffer_ = VK_NULL_HANDLE;
}

void NativeRenderTargets::destroyImage(NativeImage& image) noexcept {
    vkDestroyImageView(context_.device, image.resource.view, nullptr);
    if (image.resource.image) {
        vmaDestroyImage(allocator_, image.resource.image, image.memory);
    }
    image = {};
}

void NativeRenderTargets::destroyImages() noexcept {
    destroyFramebuffer();
    for (auto* image : {&colorAccum, &depthAccum, &colorAlpha, &depth, &normals}) {
        destroyImage(*image);
    }
}

void NativeRenderTargets::destroy() noexcept {
    destroyImages();
    for (auto renderPass : passes_) {
        vkDestroyRenderPass(context_.device, renderPass, nullptr);
    }
}

void NativeRenderTargets::createPass(bool withDepth, bool withNormals) {
    std::array<VkAttachmentDescription, 3> attachments{};
    for (auto& attachment : attachments) {
        attachment.format = floatFormat;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    const std::array<VkAttachmentReference, 3> references{{{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                                                           {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                                                           {2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}}};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1 + std::uint32_t(withDepth) + std::uint32_t(withNormals);
    subpass.pColorAttachments = references.data();
    const std::array<VkSubpassDependency, 2> dependencies{
        {{VK_SUBPASS_EXTERNAL,
          0,
          withNormals ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          withNormals ? VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                      : VK_ACCESS_SHADER_READ_BIT,
          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
          0},
         {0,
          VK_SUBPASS_EXTERNAL,
          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          withNormals ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
          VK_ACCESS_SHADER_READ_BIT,
          0}}};
    auto info = vkInfo<VkRenderPassCreateInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);
    info.attachmentCount = subpass.colorAttachmentCount;
    info.pAttachments = attachments.data();
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = static_cast<std::uint32_t>(dependencies.size());
    info.pDependencies = dependencies.data();
    const auto slot = std::size_t(withDepth) + 2 * std::size_t(withNormals);
    vkCheck(vkCreateRenderPass(context_.device, &info, nullptr, &passes_[slot]), "vkCreateRenderPass native");
}

void NativeRenderTargets::createImage(NativeImage& image,
                                      VkExtent2D extent,
                                      VkImageUsageFlags usage,
                                      FrameReport& report) {
    auto info = vkInfo<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = floatFormat;
    info.extent = {extent.width, extent.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    VmaAllocationCreateInfo memory{};
    memory.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VmaAllocationInfo allocation{};
    vkCheck(vmaCreateImage(allocator_, &info, &memory, &image.resource.image, &image.memory, &allocation),
            "vmaCreateImage native float target");
    ++report.backend.imageAllocations;
    image.bytes = allocation.size;
    image.resource.format = floatFormat;
    image.resource.extent = extent;
    image.resource.contentExtent = extent;
    auto view = vkInfo<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    view.image = image.resource.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = floatFormat;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const auto result = vkCreateImageView(context_.device, &view, nullptr, &image.resource.view);
    if (result != VK_SUCCESS) {
        destroyImage(image);
        vkCheck(result, "vkCreateImageView native float target");
    }
}

void NativeRenderTargets::createFramebuffer(VkExtent2D extent) {
    const std::array views{colorAccum.resource.view,
                           hasDepth() ? depthAccum.resource.view : normals.resource.view,
                           normals.resource.view};
    auto info = vkInfo<VkFramebufferCreateInfo>(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
    info.renderPass = pass(hasDepth(), hasNormals());
    info.attachmentCount = 1 + std::uint32_t(hasDepth()) + std::uint32_t(hasNormals());
    info.pAttachments = views.data();
    info.width = extent.width;
    info.height = extent.height;
    info.layers = 1;
    vkCheck(vkCreateFramebuffer(context_.device, &info, nullptr, &framebuffer_),
            "vkCreateFramebuffer native");
}

void NativeRenderTargets::ensure(const FrameState& frame, RenderOutput outputs, FrameReport& report) {
    const VkExtent2D extent{frame.width, frame.height};
    if (colorAccum.resource.extent.width != extent.width ||
        colorAccum.resource.extent.height != extent.height) {
        destroyImages();
    }
    const bool wantsNormals = hasOutput(outputs, RenderOutput::Normals);
    if (hasDepth() != hasOutput(outputs, RenderOutput::Depth) || hasNormals() != wantsNormals) {
        destroyFramebuffer();
    }
    if (!colorAccum.resource.image) {
        createImage(colorAccum, extent, accumulationUsage, report);
    }
    for (auto* image : {&depthAccum, &colorAlpha, &depth}) {
        const bool needed =
            hasOutput(outputs, image == &colorAlpha ? RenderOutput::FloatColorAlpha : RenderOutput::Depth);
        if (!needed) {
            destroyImage(*image);
        } else if (!image->resource.image) {
            createImage(*image, extent, image == &depthAccum ? accumulationUsage : outputUsage, report);
        }
    }
    if (!wantsNormals) {
        destroyImage(normals);
    } else if (!hasNormals()) {
        createImage(normals, extent, accumulationUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, report);
    }
    if (!framebuffer_) {
        createFramebuffer(extent);
    }
}

std::size_t NativeRenderTargets::bytes() const {
    return colorAccum.bytes + depthAccum.bytes + colorAlpha.bytes + depth.bytes + normals.bytes;
}

VulkanRenderImages NativeRenderTargets::outputImages() const {
    VulkanRenderImages result{};
    if (colorAlpha.hasContent) {
        result.floatColorAlpha = colorAlpha.resource;
    }
    if (depth.hasContent) {
        result.depth = depth.resource;
    }
    if (normals.hasContent) {
        result.normals = normals.resource;
    }
    return result;
}
}
