#include "static_gs_renderer.hpp"
#include "native_radix_sort.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace orbelio {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::uint32_t projectGroupSize = 256;
constexpr std::uint32_t composeGroupSize = 16;
constexpr std::uint32_t timestampCount = 5;
constexpr std::uint32_t quadVertices = 4;
constexpr std::size_t positionBytes = 4 * sizeof(float);
constexpr std::size_t orientationBytes = 8 * sizeof(float);
constexpr std::size_t projectedBytes = 12 * sizeof(float);
constexpr std::size_t rgbaPixelBytes = 4;
constexpr std::size_t accumulationPixelBytes = 4 * sizeof(float);
constexpr VkFormat accumulationFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
constexpr auto counterInterval = std::chrono::milliseconds(250);
constexpr auto fenceWaitNs = std::numeric_limits<std::uint64_t>::max();
constexpr double fullTurnDegrees = 360.;
constexpr VkBufferUsageFlags physicalBufferUsage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
constexpr VkBufferUsageFlags modelBufferUsage = physicalBufferUsage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

struct alignas(16) ProjectParameters {
    std::array<float, 4> right{};
    std::array<float, 4> down{};
    std::array<float, 4> forward{};
    std::array<float, 4> eye{};
    std::array<float, 4> viewport{};
    std::array<std::uint32_t, 4> controls{};
    std::array<VkDeviceAddress, 4> addresses{};
};

static_assert(sizeof(ProjectParameters) == 128);

struct GraphicsParameters {
    VkDeviceAddress projected;
    std::array<float, 2> viewport;
};

static_assert(sizeof(GraphicsParameters) == 16);

double milliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

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

void transition(VkCommandBuffer command,
                const VkImageMemoryBarrier& barrier,
                VkPipelineStageFlags before,
                VkPipelineStageFlags after) {
    vkCmdPipelineBarrier(command, before, after, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

VkPipelineColorBlendAttachmentState blendState() {
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                           VK_COLOR_COMPONENT_A_BIT;
    return blend;
}
}

struct StaticGsRenderer::Impl {
    DeviceContext context;
    VmaAllocator allocator;
    NativeBufferCounts counts;
    NativeRadixSort sorter;
    NativePipeline projection;
    NativePipeline raster;
    NativePipeline compose;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkCommandPool commands = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkQueryPool queries = VK_NULL_HANDLE;
    double timestampPeriod = 0;
    std::uint64_t timestampMask = 0;
    VkImage rasterImage = VK_NULL_HANDLE;
    VkImageView rasterView = VK_NULL_HANDLE;
    VmaAllocation rasterMemory = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    std::uint32_t rasterWidth = 0;
    std::uint32_t rasterHeight = 0;
    NativeBuffer positions;
    NativeBuffer orientations;
    NativeBuffer harmonics;
    NativeBuffer projected;
    NativeBuffer keys;
    NativeBuffer indices;
    NativeBuffer staging;
    NativeBuffer counter;
    NativeBuffer counterReadback;
    NativeBuffer pixelReadback;
    std::shared_ptr<const StaticScene> scene;
    std::shared_ptr<const FrameCapture> capture;
    std::size_t targetCount = 0;
    std::size_t uploadedCount = 0;
    std::uint64_t uploads = 0;
    std::uint64_t pixelReadbacks = 0;
    Clock::time_point lastCounterSample;

    Impl(DeviceContext c, VmaAllocator a, std::string_view directory)
        : context(c), allocator(a), sorter(c, a, counts, directory) {
        try {
            createCommands();
            createRenderPass();
            createSampler();
            createPipelines(directory);
            createQueries();
        } catch (...) {
            destroy();
            throw;
        }
    }

    ~Impl() { destroy(); }

    void destroyRasterTarget() noexcept {
        vkDestroyFramebuffer(context.device, framebuffer, nullptr);
        vkDestroyImageView(context.device, rasterView, nullptr);
        if (rasterImage) {
            vmaDestroyImage(allocator, rasterImage, rasterMemory);
        }
        framebuffer = VK_NULL_HANDLE;
        rasterView = VK_NULL_HANDLE;
        rasterImage = VK_NULL_HANDLE;
        rasterMemory = VK_NULL_HANDLE;
        rasterWidth = 0;
        rasterHeight = 0;
    }

    void destroy() noexcept {
        destroyRasterTarget();
        for (auto* buffer : {&positions,
                             &orientations,
                             &harmonics,
                             &projected,
                             &keys,
                             &indices,
                             &staging,
                             &counter,
                             &counterReadback,
                             &pixelReadback}) {
            destroyBuffer(allocator, *buffer);
        }
        for (auto* pipeline : {&projection, &raster, &compose}) {
            destroyPipeline(context, *pipeline);
        }
        vkDestroyQueryPool(context.device, queries, nullptr);
        vkDestroyFence(context.device, fence, nullptr);
        vkDestroyCommandPool(context.device, commands, nullptr);
        vkDestroySampler(context.device, sampler, nullptr);
        vkDestroyRenderPass(context.device, renderPass, nullptr);
    }

    void createCommands() {
        auto pool = vkInfo<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
        pool.queueFamilyIndex = context.queueFamily;
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        vkCheck(vkCreateCommandPool(context.device, &pool, nullptr, &commands), "vkCreateCommandPool native");
        auto allocation = vkInfo<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
        allocation.commandPool = commands;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        vkCheck(vkAllocateCommandBuffers(context.device, &allocation, &command),
                "vkAllocateCommandBuffers native");
        auto info = vkInfo<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
        vkCheck(vkCreateFence(context.device, &info, nullptr, &fence), "vkCreateFence native");
    }

    void createRenderPass() {
        VkFormatProperties supported{};
        vkGetPhysicalDeviceFormatProperties(context.physicalDevice, accumulationFormat, &supported);
        if (!(supported.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)) {
            throw std::runtime_error("Native Gaussian rendering requires RGBA32F color blending");
        }
        VkAttachmentDescription attachment{};
        attachment.format = accumulationFormat;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        const VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &reference;
        const std::array<VkSubpassDependency, 2> dependencies{{{VK_SUBPASS_EXTERNAL,
                                                                0,
                                                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                                VK_ACCESS_SHADER_READ_BIT,
                                                                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                                                0},
                                                               {0,
                                                                VK_SUBPASS_EXTERNAL,
                                                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                                                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                                                VK_ACCESS_SHADER_READ_BIT,
                                                                0}}};
        auto info = vkInfo<VkRenderPassCreateInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);
        info.attachmentCount = 1;
        info.pAttachments = &attachment;
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        info.dependencyCount = static_cast<std::uint32_t>(dependencies.size());
        info.pDependencies = dependencies.data();
        vkCheck(vkCreateRenderPass(context.device, &info, nullptr, &renderPass), "vkCreateRenderPass native");
    }

    void createSampler() {
        auto info = vkInfo<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
        info.magFilter = VK_FILTER_NEAREST;
        info.minFilter = VK_FILTER_NEAREST;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCheck(vkCreateSampler(context.device, &info, nullptr, &sampler), "vkCreateSampler native");
    }

    void createGraphicsPipeline(std::string_view directory) {
        const std::array types{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
        createNativeLayout(context, raster, types, VK_SHADER_STAGE_VERTEX_BIT, sizeof(GraphicsParameters));
        const auto vertex = loadNativeShader(context, directory, "native_quad.vert.spv");
        VkShaderModule fragment = VK_NULL_HANDLE;
        try {
            fragment = loadNativeShader(context, directory, "native_quad.frag.spv");
            createGraphicsStages(vertex, fragment);
        } catch (...) {
            vkDestroyShaderModule(context.device, vertex, nullptr);
            vkDestroyShaderModule(context.device, fragment, nullptr);
            throw;
        }
        vkDestroyShaderModule(context.device, vertex, nullptr);
        vkDestroyShaderModule(context.device, fragment, nullptr);
    }

    void createGraphicsStages(VkShaderModule vertex, VkShaderModule fragment) {
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        for (auto& stage : stages) {
            stage =
                vkInfo<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
            stage.pName = "main";
        }
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment;
        auto input = vkInfo<VkPipelineVertexInputStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
        auto assembly = vkInfo<VkPipelineInputAssemblyStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        auto viewport =
            vkInfo<VkPipelineViewportStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        auto rasterization = vkInfo<VkPipelineRasterizationStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
        rasterization.polygonMode = VK_POLYGON_MODE_FILL;
        rasterization.cullMode = VK_CULL_MODE_NONE;
        rasterization.lineWidth = 1;
        auto sampling = vkInfo<VkPipelineMultisampleStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
        sampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        const auto attachment = blendState();
        auto blending = vkInfo<VkPipelineColorBlendStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
        blending.attachmentCount = 1;
        blending.pAttachments = &attachment;
        const std::array dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        auto dynamic =
            vkInfo<VkPipelineDynamicStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
        dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
        dynamic.pDynamicStates = dynamicStates.data();
        auto info = vkInfo<VkGraphicsPipelineCreateInfo>(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
        info.stageCount = static_cast<std::uint32_t>(stages.size());
        info.pStages = stages.data();
        info.pVertexInputState = &input;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &rasterization;
        info.pMultisampleState = &sampling;
        info.pColorBlendState = &blending;
        info.pDynamicState = &dynamic;
        info.layout = raster.layout;
        info.renderPass = renderPass;
        vkCheck(vkCreateGraphicsPipelines(context.device, VK_NULL_HANDLE, 1, &info, nullptr, &raster.handle),
                "vkCreateGraphicsPipelines native");
    }

    void createPipelines(std::string_view directory) {
        createNativeCompute(
            context, projection, directory, "native_project.comp.spv", 3, sizeof(ProjectParameters));
        createGraphicsPipeline(directory);
        const std::array types{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
        createNativeLayout(context, compose, types, VK_SHADER_STAGE_COMPUTE_BIT, 0);
        const auto shader = loadNativeShader(context, directory, "native_compose.comp.spv");
        auto info = vkInfo<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
        info.layout = compose.layout;
        info.stage =
            vkInfo<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = shader;
        info.stage.pName = "main";
        const auto result =
            vkCreateComputePipelines(context.device, VK_NULL_HANDLE, 1, &info, nullptr, &compose.handle);
        vkDestroyShaderModule(context.device, shader, nullptr);
        vkCheck(result, "vkCreateComputePipelines native compose");
    }

    void createQueries() {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(context.physicalDevice, &properties);
        timestampPeriod = properties.limits.timestampPeriod;
        std::uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(context.physicalDevice, &count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(count);
        vkGetPhysicalDeviceQueueFamilyProperties(context.physicalDevice, &count, queues.data());
        const auto bits = queues[context.queueFamily].timestampValidBits;
        if (!bits || !timestampPeriod) {
            return;
        }
        timestampMask =
            bits == 64 ? std::numeric_limits<std::uint64_t>::max() : (std::uint64_t{1} << bits) - 1;
        auto info = vkInfo<VkQueryPoolCreateInfo>(VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);
        info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = timestampCount;
        vkCheck(vkCreateQueryPool(context.device, &info, nullptr, &queries), "vkCreateQueryPool native");
    }

    void ensureRasterTarget(const FrameState& frame, FrameReport& report) {
        if (rasterWidth == frame.width && rasterHeight == frame.height) {
            return;
        }
        destroyRasterTarget();
        auto info = vkInfo<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = accumulationFormat;
        info.extent = {frame.width, frame.height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        VmaAllocationCreateInfo memory{};
        memory.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        vkCheck(vmaCreateImage(allocator, &info, &memory, &rasterImage, &rasterMemory, nullptr),
                "vmaCreateImage native accumulation");
        ++report.backend.imageAllocations;
        auto view = vkInfo<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
        view.image = rasterImage;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = accumulationFormat;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCheck(vkCreateImageView(context.device, &view, nullptr, &rasterView), "vkCreateImageView native");
        auto target = vkInfo<VkFramebufferCreateInfo>(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
        target.renderPass = renderPass;
        target.attachmentCount = 1;
        target.pAttachments = &rasterView;
        target.width = frame.width;
        target.height = frame.height;
        target.layers = 1;
        vkCheck(vkCreateFramebuffer(context.device, &target, nullptr, &framebuffer),
                "vkCreateFramebuffer native");
        rasterWidth = frame.width;
        rasterHeight = frame.height;
    }

    bool ensureScene(const FrameState& frame) {
        const auto nextCount = frame.renderSettings.budgetedCount(frame.scene->count());
        const bool changed = scene != frame.scene || targetCount != nextCount;
        if (changed) {
            scene = frame.scene;
            targetCount = nextCount;
            uploadedCount = 0;
            capture.reset();
            ++uploads;
        }
        return changed;
    }

    void ensureBuffers() {
        reserveBuffer(allocator,
                      positions,
                      targetCount * positionBytes,
                      modelBufferUsage,
                      NativeHostAccess::None,
                      counts);
        reserveBuffer(allocator,
                      orientations,
                      targetCount * orientationBytes,
                      modelBufferUsage,
                      NativeHostAccess::None,
                      counts);
        const auto shBytes = (scene->shRestCoefficients() + 1) * 3 * sizeof(float);
        reserveBuffer(
            allocator, harmonics, targetCount * shBytes, modelBufferUsage, NativeHostAccess::None, counts);
        reserveBuffer(allocator,
                      projected,
                      targetCount * projectedBytes,
                      physicalBufferUsage,
                      NativeHostAccess::None,
                      counts);
        reserveBuffer(allocator,
                      keys,
                      targetCount * sizeof(std::uint32_t),
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      NativeHostAccess::None,
                      counts);
        reserveBuffer(allocator,
                      indices,
                      targetCount * sizeof(std::uint32_t),
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      NativeHostAccess::None,
                      counts);
        reserveBuffer(allocator,
                      counter,
                      sizeof(std::uint32_t),
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      NativeHostAccess::None,
                      counts);
        reserveBuffer(allocator,
                      counterReadback,
                      sizeof(std::uint32_t),
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      NativeHostAccess::Readback,
                      counts);
        sorter.reserve(static_cast<std::uint32_t>(targetCount));
    }

    void begin() {
        vkCheck(vkResetCommandPool(context.device, commands, 0), "vkResetCommandPool native");
        auto info = vkInfo<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
        info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkCheck(vkBeginCommandBuffer(command, &info), "vkBeginCommandBuffer native");
        if (queries) {
            vkCmdResetQueryPool(command, queries, 0, timestampCount);
        }
    }

    void packUpload(std::size_t end, std::size_t shBytes) {
        const auto points = end - uploadedCount;
        auto* bytes = static_cast<std::byte*>(staging.mapped);
        for (std::size_t i = 0; i < points; ++i) {
            const auto source = sampledGaussian(uploadedCount + i, scene->count(), targetCount);
            const std::array<float, 4> position{scene->positions[source * 3],
                                                scene->positions[source * 3 + 1],
                                                scene->positions[source * 3 + 2],
                                                scene->opacityLogits[source]};
            std::memcpy(bytes + i * positionBytes, position.data(), positionBytes);
            auto* orient = bytes + points * positionBytes + i * orientationBytes;
            std::memcpy(orient, scene->rotations.data() + source * 4, 4 * sizeof(float));
            const std::array<float, 4> scale{scene->logScales[source * 3],
                                             scene->logScales[source * 3 + 1],
                                             scene->logScales[source * 3 + 2],
                                             0};
            std::memcpy(orient + 4 * sizeof(float), scale.data(), 4 * sizeof(float));
            auto* sh = bytes + points * (positionBytes + orientationBytes) + i * shBytes;
            std::memcpy(sh, scene->sh0.data() + source * 3, 3 * sizeof(float));
            if (scene->shRestCoefficients()) {
                std::memcpy(sh + 3 * sizeof(float),
                            scene->shRest.data() + source * scene->shRestCoefficients() * 3,
                            shBytes - 3 * sizeof(float));
            }
        }
    }

    void upload(const FrameState& frame) {
        const auto batch = frame.renderSettings.uploadGaussiansPerFrame;
        const auto end = batch ? std::min(targetCount, uploadedCount + batch) : targetCount;
        if (end == uploadedCount) {
            return;
        }
        const auto points = end - uploadedCount;
        const auto shBytes = (scene->shRestCoefficients() + 1) * 3 * sizeof(float);
        const auto bytes = points * (positionBytes + orientationBytes + shBytes);
        reserveBuffer(
            allocator, staging, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, NativeHostAccess::Upload, counts);
        packUpload(end, shBytes);
        vkCheck(vmaFlushAllocation(allocator, staging.allocation, 0, bytes),
                "vmaFlushAllocation native upload");
        const std::array<const NativeBuffer*, 3> targets{&positions, &orientations, &harmonics};
        const std::array strides{positionBytes, orientationBytes, shBytes};
        std::size_t offset = 0;
        for (std::size_t i = 0; i < targets.size(); ++i) {
            const VkBufferCopy copy{offset, uploadedCount * strides[i], points * strides[i]};
            vkCmdCopyBuffer(command, staging.handle, targets[i]->handle, 1, &copy);
            offset += points * strides[i];
        }
        uploadedCount = end;
    }

    ProjectParameters parameters(const FrameState& frame, bool sample) const {
        const auto camera = cameraView(frame.camera, frame.width, frame.height);
        ProjectParameters result{};
        std::copy(camera.right.begin(), camera.right.end(), result.right.begin());
        std::copy(camera.down.begin(), camera.down.end(), result.down.begin());
        std::copy(camera.forward.begin(), camera.forward.end(), result.forward.begin());
        std::copy(camera.eye.begin(), camera.eye.end(), result.eye.begin());
        const auto& settings = frame.renderSettings;
        const bool scale = settings.attributeView == AttributeView::Scale;
        result.right[3] = scale ? std::log(settings.attributeMinimum) : settings.attributeMinimum;
        result.down[3] = scale ? std::log(settings.attributeMaximum) : settings.attributeMaximum;
        result.viewport = {float(frame.width), float(frame.height), camera.focalPixels, 0};
        result.controls = {static_cast<std::uint32_t>(uploadedCount),
                           scene->shDegree,
                           static_cast<std::uint32_t>(settings.attributeView),
                           std::uint32_t(scene->antialiased) | (std::uint32_t(sample) << 1)};
        result.addresses = {bufferAddress(context, positions),
                            bufferAddress(context, orientations),
                            bufferAddress(context, harmonics),
                            bufferAddress(context, projected)};
        return result;
    }

    void stamp(std::uint32_t index) {
        if (queries) {
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, queries, index);
        }
    }

    void project(const FrameState& frame, bool sample) {
        if (sample) {
            vkCmdFillBuffer(command, counter.handle, 0, sizeof(std::uint32_t), 0);
        }
        nativeMemoryBarrier(command,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        stamp(0);
        bindNativeBuffers(command, projection, VK_PIPELINE_BIND_POINT_COMPUTE, {&keys, &indices, &counter});
        const auto settings = parameters(frame, sample);
        vkCmdPushConstants(
            command, projection.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(settings), &settings);
        vkCmdDispatch(command,
                      (static_cast<std::uint32_t>(uploadedCount) + projectGroupSize - 1) / projectGroupSize,
                      1,
                      1);
        nativeMemoryBarrier(command,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            VK_ACCESS_SHADER_WRITE_BIT,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        stamp(1);
    }

    void rasterize(const FrameState& frame) {
        nativeMemoryBarrier(command,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            VK_ACCESS_SHADER_WRITE_BIT,
                            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                            VK_ACCESS_SHADER_READ_BIT);
        const VkClearValue clear{{{0, 0, 0, 1}}};
        auto pass = vkInfo<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
        pass.renderPass = renderPass;
        pass.framebuffer = framebuffer;
        pass.renderArea.extent = {frame.width, frame.height};
        pass.clearValueCount = 1;
        pass.pClearValues = &clear;
        vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
        bindNativeBuffers(command, raster, VK_PIPELINE_BIND_POINT_GRAPHICS, {&indices});
        const GraphicsParameters settings{bufferAddress(context, projected),
                                          {float(frame.width), float(frame.height)}};
        vkCmdPushConstants(
            command, raster.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(settings), &settings);
        const VkViewport viewport{0, 0, float(frame.width), float(frame.height), 0, 1};
        const VkRect2D scissor{{0, 0}, {frame.width, frame.height}};
        vkCmdSetViewport(command, 0, 1, &viewport);
        vkCmdSetScissor(command, 0, 1, &scissor);
        vkCmdDraw(command, quadVertices, static_cast<std::uint32_t>(uploadedCount), 0, 0);
        vkCmdEndRenderPass(command);
        stamp(3);
    }

    void composeImage(const FrameState& frame, VkImage image, VkImageView view, bool initialized) {
        auto toWritable =
            imageBarrier(image,
                         initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_GENERAL);
        toWritable.srcAccessMask = initialized ? VK_ACCESS_SHADER_READ_BIT : 0;
        toWritable.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        transition(command,
                   toWritable,
                   initialized ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        const std::array<VkDescriptorImageInfo, 2> infos{
            {{sampler, rasterView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
             {VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL}}};
        std::array<VkWriteDescriptorSet, 2> writes{};
        for (std::uint32_t i = 0; i < writes.size(); ++i) {
            writes[i] = vkInfo<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType =
                i ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = &infos[i];
        }
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, compose.handle);
        vkCmdPushDescriptorSetKHR(command,
                                  VK_PIPELINE_BIND_POINT_COMPUTE,
                                  compose.layout,
                                  0,
                                  static_cast<std::uint32_t>(writes.size()),
                                  writes.data());
        vkCmdDispatch(command,
                      (frame.width + composeGroupSize - 1) / composeGroupSize,
                      (frame.height + composeGroupSize - 1) / composeGroupSize,
                      1);
        stamp(4);
    }

    void recordCapture(const FrameState& frame, VkImage image) {
        auto toCopy = imageBarrier(image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        toCopy.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        toCopy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        transition(command, toCopy, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {frame.width, frame.height, 1};
        vkCmdCopyImageToBuffer(
            command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pixelReadback.handle, 1, &copy);
    }

    void finishRecording(VkImage image, bool sampled, bool captured) {
        if (sampled) {
            nativeMemoryBarrier(command,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                VK_ACCESS_SHADER_WRITE_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_ACCESS_TRANSFER_READ_BIT);
            const VkBufferCopy copy{0, 0, sizeof(std::uint32_t)};
            vkCmdCopyBuffer(command, counter.handle, counterReadback.handle, 1, &copy);
        }
        if (sampled || captured) {
            nativeMemoryBarrier(command,
                                VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_ACCESS_TRANSFER_WRITE_BIT,
                                VK_PIPELINE_STAGE_HOST_BIT,
                                VK_ACCESS_HOST_READ_BIT);
        }
        auto readable =
            imageBarrier(image,
                         captured ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        readable.srcAccessMask = captured ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_SHADER_WRITE_BIT;
        readable.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        transition(command,
                   readable,
                   captured ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        vkCheck(vkEndCommandBuffer(command), "vkEndCommandBuffer native");
    }

    double submitAndWait() {
        vkCheck(vkResetFences(context.device, 1, &fence), "vkResetFences native");
        auto info = vkInfo<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        info.commandBufferCount = 1;
        info.pCommandBuffers = &command;
        vkCheck(vkQueueSubmit(context.queue, 1, &info, fence), "vkQueueSubmit native");
        const auto start = Clock::now();
        vkCheck(vkWaitForFences(context.device, 1, &fence, VK_TRUE, fenceWaitNs), "vkWaitForFences native");
        return milliseconds(start);
    }

    void readTimestamps(FrameStats& frame) {
        frame.projectionGpuMs.reset();
        frame.sortingGpuMs.reset();
        frame.rasterGpuMs.reset();
        frame.composeGpuMs.reset();
        frame.gpuRenderMs.reset();
        if (!queries) {
            return;
        }
        std::array<std::uint64_t, timestampCount> values{};
        vkCheck(vkGetQueryPoolResults(context.device,
                                      queries,
                                      0,
                                      timestampCount,
                                      sizeof(values),
                                      values.data(),
                                      sizeof(values[0]),
                                      VK_QUERY_RESULT_64_BIT),
                "vkGetQueryPoolResults native");
        const auto interval = [&](std::size_t i) {
            return double((values[i + 1] - values[i]) & timestampMask) * timestampPeriod / 1'000'000.;
        };
        frame.projectionGpuMs = interval(0);
        frame.sortingGpuMs = interval(1);
        frame.rasterGpuMs = interval(2);
        frame.composeGpuMs = interval(3);
        frame.gpuRenderMs = *frame.projectionGpuMs + *frame.sortingGpuMs + *frame.rasterGpuMs;
    }

    void readResults(const FrameState& frame, FrameReport& report, bool sample, bool captured) {
        const auto start = Clock::now();
        readTimestamps(report.frame);
        if (sample) {
            vkCheck(vmaInvalidateAllocation(allocator, counterReadback.allocation, 0, sizeof(std::uint32_t)),
                    "vmaInvalidateAllocation native counter");
            std::uint32_t visible = 0;
            std::memcpy(&visible, counterReadback.mapped, sizeof(visible));
            report.backend.visibleGaussians = visible;
            report.backend.telemetryFrameIndex = report.frame.frameIndex + 1;
            lastCounterSample = Clock::now();
        }
        report.backend.telemetryAgeMs = milliseconds(lastCounterSample);
        report.frame.queryReadbackCpuMs = milliseconds(start);
        if (captured) {
            const auto bytes = std::size_t(frame.width) * frame.height * rgbaPixelBytes;
            vkCheck(vmaInvalidateAllocation(allocator, pixelReadback.allocation, 0, bytes),
                    "vmaInvalidateAllocation native pixels");
            auto result = std::make_shared<FrameCapture>();
            result->requestId = frame.captureRequestId;
            result->width = frame.width;
            result->height = frame.height;
            result->rgba.resize(bytes);
            std::memcpy(result->rgba.data(), pixelReadback.mapped, bytes);
            capture = std::move(result);
            ++pixelReadbacks;
        }
    }

    void fillReport(const FrameState& state, FrameReport& report) const {
        auto& frame = report.frame;
        frame.gaussianCount = scene->count();
        frame.submittedGaussians = uploadedCount;
        frame.budgetedGaussians = targetCount;
        frame.refining = uploadedCount < targetCount;
        frame.sceneUploads = uploads;
        frame.pixelReadbacks = pixelReadbacks;
        frame.orbitDegrees = std::fmod(state.camera.yawDegrees + fullTurnDegrees, fullTurnDegrees);
        frame.capture = capture;
        frame.temporalEvaluationCpuMs.reset();
        auto& stats = report.backend;
        stats.rasterizer = "native-vulkan-ewa-quads";
        stats.gsBufferAllocations = counts.allocations;
        stats.gsBufferReuses = counts.reuses;
        stats.gsGrowths = counts.growths;
        stats.modelBytes = positions.capacity + orientations.capacity + harmonics.capacity + staging.capacity;
        stats.preprocessingBytes = projected.capacity;
        stats.sortingBytes = keys.capacity + indices.capacity + sorter.bytes();
        stats.pixelBytes = std::size_t(rasterWidth) * rasterHeight * accumulationPixelBytes;
        stats.telemetryBytes = counter.capacity + counterReadback.capacity + pixelReadback.capacity;
        stats.gsReservedBytes = stats.modelBytes + stats.preprocessingBytes + stats.sortingBytes +
                                stats.pixelBytes + stats.telemetryBytes;
    }

    void render(const FrameState& frame,
                VkImage image,
                VkImageView view,
                bool initialized,
                FrameReport& report) {
        const bool changed = ensureScene(frame);
        ensureRasterTarget(frame, report);
        ensureBuffers();
        begin();
        const bool wasRefining = uploadedCount < targetCount;
        upload(frame);
        const bool sample = changed || (wasRefining && uploadedCount == targetCount) ||
                            Clock::now() - lastCounterSample >= counterInterval;
        const bool captured = frame.captureRequestId && uploadedCount == targetCount &&
                              (!capture || capture->requestId != frame.captureRequestId);
        if (captured) {
            const auto bytes = std::bit_ceil(std::size_t(frame.width) * frame.height * rgbaPixelBytes);
            reserveBuffer(allocator,
                          pixelReadback,
                          bytes,
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          NativeHostAccess::Readback,
                          counts);
        }
        project(frame, sample);
        sorter.record(command, static_cast<std::uint32_t>(uploadedCount), keys, indices);
        stamp(2);
        rasterize(frame);
        composeImage(frame, image, view, initialized);
        if (captured) {
            recordCapture(frame, image);
        }
        finishRecording(image, sample, captured);
        report.frame.backendWaitMs = submitAndWait();
        readResults(frame, report, sample, captured);
        fillReport(frame, report);
    }
};

StaticGsRenderer::StaticGsRenderer(DeviceContext context, VmaAllocator allocator, std::string directory)
    : impl_(std::make_unique<Impl>(context, allocator, directory)) {}

StaticGsRenderer::~StaticGsRenderer() = default;

void StaticGsRenderer::render(const FrameState& frame,
                              VkImage image,
                              VkImageView view,
                              bool initialized,
                              FrameReport& report) {
    impl_->render(frame, image, view, initialized, report);
}
}
