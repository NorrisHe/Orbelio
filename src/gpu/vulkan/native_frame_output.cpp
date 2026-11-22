#include "native_frame_output.hpp"
#include <algorithm>

namespace orbelio {
namespace {
constexpr std::uint32_t composeGroupSize = 16;

struct alignas(16) ComposeParameters {
    std::array<float, 4> background{};
    std::array<std::uint32_t, 4> controls{};
};

static_assert(sizeof(ComposeParameters) == 32);

VkImageMemoryBarrier imageBarrier(VkImage image, VkImageLayout before, VkImageLayout after) {
    auto barrier = vkInfo<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
    barrier.image = image;
    barrier.oldLayout = before;
    barrier.newLayout = after;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return barrier;
}

}

NativeFrameOutput::NativeFrameOutput(DeviceContext context,
                                     VmaAllocator allocator,
                                     NativeBufferCounts& counts,
                                     std::string_view directory)
    : context_(context), allocator_(allocator), counts_(counts), targets_(context, allocator) {
    try {
        createSampler();
        createComposePipeline(directory, compose_, false);
        createComposePipeline(directory, composeFloat_, true);
    } catch (...) {
        destroy();
        throw;
    }
}

NativeFrameOutput::~NativeFrameOutput() { destroy(); }

void NativeFrameOutput::destroy() noexcept {
    destroyBuffer(allocator_, pixelReadback_);
    destroyPipeline(context_, compose_);
    destroyPipeline(context_, composeFloat_);
    vkDestroySampler(context_.device, sampler_, nullptr);
}

void NativeFrameOutput::createSampler() {
    auto info = vkInfo<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
    info.magFilter = VK_FILTER_NEAREST;
    info.minFilter = VK_FILTER_NEAREST;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCheck(vkCreateSampler(context_.device, &info, nullptr, &sampler_), "vkCreateSampler native");
}

void NativeFrameOutput::createComposePipeline(std::string_view directory,
                                              NativePipeline& pipeline,
                                              bool withFloat) {
    const std::array types{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                           VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                           VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    createNativeCompute(context_,
                        pipeline,
                        directory,
                        withFloat ? "native_compose_float.comp.spv" : "native_compose.comp.spv",
                        std::span(types).first(withFloat ? 5 : 2),
                        sizeof(ComposeParameters));
}

static void makeWritable(VkCommandBuffer command, VkImage image, bool initialized) {
    auto barrier =
        imageBarrier(image,
                     initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_GENERAL);
    barrier.srcAccessMask = initialized ? VK_ACCESS_SHADER_READ_BIT : 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    nativeImageBarrier(command,
                       initialized ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       barrier);
}

void NativeFrameOutput::bindComposeImages(VkCommandBuffer command,
                                          const NativePipeline& pipeline,
                                          VkImageView display,
                                          bool withFloat) {
    const auto& fallback = targets_.colorAlpha.resource.image ? targets_.colorAlpha : targets_.depth;
    const auto& color = targets_.colorAlpha.resource.image ? targets_.colorAlpha : fallback;
    const auto& depth = targets_.depth.resource.image ? targets_.depth : fallback;
    const auto depthSource =
        targets_.hasDepth() ? targets_.depthAccum.resource.view : targets_.colorAccum.resource.view;
    const std::array<VkDescriptorImageInfo, 5> infos{
        {{sampler_, targets_.colorAccum.resource.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
         {VK_NULL_HANDLE, display, VK_IMAGE_LAYOUT_GENERAL},
         {VK_NULL_HANDLE, color.resource.view, VK_IMAGE_LAYOUT_GENERAL},
         {sampler_, depthSource, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
         {VK_NULL_HANDLE, depth.resource.view, VK_IMAGE_LAYOUT_GENERAL}}};
    std::array<VkWriteDescriptorSet, 5> writes{};
    const std::uint32_t count = withFloat ? 5 : 2;
    for (std::uint32_t i = 0; i < count; ++i) {
        writes[i] = vkInfo<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType =
            (i == 0 || i == 3) ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo = &infos[i];
    }
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.handle);
    vkCmdPushDescriptorSetKHR(
        command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.layout, 0, count, writes.data());
}

void NativeFrameOutput::composeImage(VkCommandBuffer command,
                                     const FrameState& frame,
                                     NativeDisplayTarget display) {
    makeWritable(command, display.image, display.initialized);
    RenderOutput outputs = RenderOutput::Display;
    for (auto* target : {&targets_.colorAlpha, &targets_.depth}) {
        if (target->resource.image) {
            makeWritable(command, target->resource.image, target->hasContent);
            outputs =
                outputs | (target == &targets_.depth ? RenderOutput::Depth : RenderOutput::FloatColorAlpha);
        }
    }
    const bool withFloat = outputs != RenderOutput::Display;
    const auto& pipeline = withFloat ? composeFloat_ : compose_;
    bindComposeImages(command, pipeline, display.view, withFloat);
    ComposeParameters settings{};
    std::copy(frame.renderSettings.background.begin(),
              frame.renderSettings.background.end(),
              settings.background.begin());
    settings.controls[0] = std::uint32_t(outputs);
    vkCmdPushConstants(command, pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(settings), &settings);
    vkCmdDispatch(command,
                  (frame.width + composeGroupSize - 1) / composeGroupSize,
                  (frame.height + composeGroupSize - 1) / composeGroupSize,
                  1);
}

VkImage NativeFrameOutput::captureImage(RenderOutput output, VkImage display) const {
    if (output == RenderOutput::Display) {
        return display;
    }
    if (output == RenderOutput::Normals) {
        return targets_.normals.resource.image;
    }
    return output == RenderOutput::Depth ? targets_.depth.resource.image : targets_.colorAlpha.resource.image;
}

void NativeFrameOutput::recordCapture(VkCommandBuffer command,
                                      const FrameState& frame,
                                      VkImage display,
                                      const NativeCapturePlan& plan) {
    for (std::size_t i = 0; i < plan.count; ++i) {
        const auto& plane = plan.planes[i];
        const auto image = captureImage(plane.output, display);
        const bool normals = plane.output == RenderOutput::Normals;
        auto barrier =
            imageBarrier(image,
                         normals ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        barrier.srcAccessMask =
            VK_ACCESS_SHADER_WRITE_BIT | (normals ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : 0);
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        nativeImageBarrier(command,
                           normals ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT
                                   : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                           barrier);
        VkBufferImageCopy copy{};
        copy.bufferOffset = plane.offset;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {frame.width, frame.height, 1};
        vkCmdCopyImageToBuffer(
            command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pixelReadback_.handle, 1, &copy);
    }
}

static void makeReadable(VkCommandBuffer command, VkImage image, bool captured) {
    auto barrier = imageBarrier(image,
                                captured ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    barrier.srcAccessMask = captured ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    nativeImageBarrier(command,
                       captured ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       barrier);
}

void NativeFrameOutput::reserveCapture(const FrameState& frame, const NativeCapturePlan& plan) {
    if (!plan.count) {
        return;
    }
    if (pixelReadback_.capacity > frame.captureBudgetBytes) {
        destroyBuffer(allocator_, pixelReadback_);
    }
    reserveBuffer(allocator_,
                  pixelReadback_,
                  plan.bytes,
                  VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                  NativeHostAccess::Readback,
                  counts_);
}

void NativeFrameOutput::reserveTargets(const FrameState& frame, bool capture, FrameReport& report) {
    targets_.ensure(
        frame, frame.renderSettings.outputs | (capture ? frame.captureOutputs : RenderOutput(0)), report);
}

bool NativeFrameOutput::capturePending(const FrameState& frame) const {
    return frame.captureRequestId && (!capture_ || capture_->requestId != frame.captureRequestId ||
                                      capture_->outputs != frame.captureOutputs);
}

void NativeFrameOutput::publishLayouts(VkCommandBuffer command, VkImage display, RenderOutput captured) {
    makeReadable(command, display, hasOutput(captured, RenderOutput::Display));
    for (auto* target : {&targets_.colorAlpha, &targets_.depth}) {
        if (!target->resource.image) {
            continue;
        }
        const auto plane = target == &targets_.depth ? RenderOutput::Depth : RenderOutput::FloatColorAlpha;
        makeReadable(command, target->resource.image, hasOutput(captured, plane));
    }
    if (hasOutput(captured, RenderOutput::Normals)) {
        makeReadable(command, targets_.normals.resource.image, true);
    }
}

void NativeFrameOutput::completeSubmission() {
    for (auto* target : {&targets_.colorAlpha, &targets_.depth, &targets_.normals}) {
        target->hasContent = target->resource.image != VK_NULL_HANDLE;
        if (target->hasContent) {
            target->resource.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
    }
}

void NativeFrameOutput::readCapture(const FrameState& frame, const NativeCapturePlan& plan) {
    if (plan.count) {
        vkCheck(vmaInvalidateAllocation(allocator_, pixelReadback_.allocation, 0, plan.bytes),
                "vmaInvalidateAllocation native pixels");
        capture_ = nativeReadCapture(frame, plan, pixelReadback_.mapped);
        pixelReadbacks_ += plan.count;
    }
}

void NativeFrameOutput::fillCaptureStats(FrameStats& stats) const {
    stats.capture = capture_;
    stats.pixelReadbacks = pixelReadbacks_;
}
}
