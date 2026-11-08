#include "native_gaussian_renderer.hpp"
#include "native_radix_sort.hpp"
#include "native_render_targets.hpp"
#include "native_capture.hpp"
#include "native_tile_raster.hpp"
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
// Rejected sentinels no longer extend the domain of positive FP32 camera depths.
constexpr std::uint32_t positiveDepthKeyBits = 31;
constexpr std::uint32_t frameTimestampCount = 7;
constexpr std::uint32_t tileTimestampCount = 4;
constexpr std::uint32_t timestampCount = frameTimestampCount + tileTimestampCount;
constexpr std::size_t projectionCounterWords = 12;
constexpr std::size_t projectionCounterBytes = projectionCounterWords * sizeof(std::uint32_t);
constexpr std::size_t positionBytes = 4 * sizeof(float);
constexpr std::size_t orientationBytes = 8 * sizeof(float);
constexpr std::size_t projectedBytes = 12 * sizeof(float);
constexpr std::size_t normalBytes = 4 * sizeof(float);
constexpr auto counterInterval = std::chrono::milliseconds(250);
constexpr auto fenceWaitNs = std::numeric_limits<std::uint64_t>::max();
constexpr double fullTurnDegrees = 360.;
constexpr VkBufferUsageFlags physicalBufferUsage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
constexpr VkBufferUsageFlags modelBufferUsage = physicalBufferUsage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

bool capacityError(const VulkanError& error) {
    return error.result() == VK_ERROR_OUT_OF_DEVICE_MEMORY || error.result() == VK_ERROR_OUT_OF_HOST_MEMORY;
}

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

struct alignas(16) ProjectionSettings {
    std::array<float, 4> intrinsics{};
    std::array<float, 4> clipping{};
};

static_assert(sizeof(ProjectionSettings) == 32);

struct GraphicsParameters {
    VkDeviceAddress projected;
    std::array<float, 2> viewport;
};

static_assert(sizeof(GraphicsParameters) == 16);

struct alignas(16) ComposeParameters {
    std::array<float, 4> background{};
    std::array<std::uint32_t, 4> controls{};
};

static_assert(sizeof(ComposeParameters) == 32);

std::uint64_t counter64(const std::array<std::uint32_t, projectionCounterWords>& words, std::size_t first) {
    return std::uint64_t(words[first]) | (std::uint64_t(words[first + 1]) << 32);
}

ProjectionDiagnostics decodeDiagnostics(const std::array<std::uint32_t, projectionCounterWords>& words) {
    ProjectionDiagnostics result{};
    result.clippedBoundingPixels = counter64(words, 1);
    result.maximumBoundingPixels = words[3];
    result.maximumEllipseArea = std::bit_cast<float>(words[4]);
    result.nearOpaquePeaks = words[7];
    result.maximumAxisRadius = std::bit_cast<float>(words[8]);
    result.axisRadiusOver512 = words[9];
    if (words[0]) {
        result.meanPeakOpacity = double(counter64(words, 5)) / 65535. / words[0];
        result.meanClippedEllipseAreaUpper = double(counter64(words, 10)) / 16. / words[0];
    }
    return result;
}

struct PreparedFrame {
    CameraView camera;
    NativeCapturePlan capture;
    bool sample = false;
};

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

void validateSettings(const RenderSettings& settings) {
    if (std::uint32_t(settings.rasterMode) > std::uint32_t(RasterMode::TiledCompute) ||
        (settings.tileBinningWidth != 16 && settings.tileBinningWidth != 32 &&
         settings.tileBinningWidth != 64) ||
        !validOutputs(settings.outputs) || std::ranges::any_of(settings.background, [](float value) {
            return !std::isfinite(value) || value < 0;
        })) {
        throw std::invalid_argument("Invalid render outputs or background");
    }
}

void validateScene(const StaticScene& scene) {
    const auto count = scene.count();
    if (scene.shDegree > 3 || count > std::numeric_limits<std::uint32_t>::max() ||
        scene.positions.size() != count * 3 || scene.rotations.size() != count * 4 ||
        scene.logScales.size() != count * 3 || scene.sh0.size() != count * 3 ||
        scene.shRest.size() != count * scene.shRestCoefficients() * 3) {
        throw std::invalid_argument("Invalid Gaussian scene layout");
    }
}
}

struct NativeGaussianRenderer::Impl {
    DeviceContext context;
    VmaAllocator allocator;
    NativeBufferCounts counts;
    NativeRadixSort sorter;
    NativeRenderTargets targets;
    std::unique_ptr<NativeTileRaster> tiler;
    std::optional<std::array<std::size_t, 5>> rejectedTile;
    std::string shaderDirectory;
    NativePipeline projection;
    NativePipeline projectionNormals;
    NativePipeline shading;
    NativePipeline raster;
    NativePipeline rasterDepth;
    NativePipeline rasterNormals;
    NativePipeline rasterDepthNormals;
    NativePipeline compose;
    NativePipeline composeFloat;
    VkSampler sampler = VK_NULL_HANDLE;
    VkCommandPool commands = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkQueryPool queries = VK_NULL_HANDLE;
    VkQueryPool statisticsQueries = VK_NULL_HANDLE;
    double timestampPeriod = 0;
    std::uint64_t timestampMask = 0;
    NativeBuffer positions;
    NativeBuffer projectionSettings;
    NativeBuffer orientations;
    NativeBuffer harmonics;
    NativeBuffer projected;
    NativeBuffer projectedNormals;
    NativeBuffer keys;
    NativeBuffer indices;
    NativeBuffer visibility;
    NativeBuffer staging;
    NativeBuffer counter;
    NativeBuffer counterReadback;
    NativeBuffer pixelReadback;
    std::shared_ptr<const StaticScene> scene;
    GaussianTemporalView temporal;
    std::shared_ptr<const FrameCapture> capture;
    std::size_t targetCount = 0;
    std::size_t uploadedCount = 0;
    std::uint64_t uploads = 0;
    std::uint64_t pixelReadbacks = 0;
    Clock::time_point lastCounterSample;

    Impl(DeviceContext c, VmaAllocator a, std::string_view directory)
        : context(c), allocator(a), sorter(c, a, counts, directory, NativeSortMode::VisibleDepths),
          targets(c, a), shaderDirectory(directory) {
        try {
            createCommands();
            createSampler();
            createPipelines(directory);
            createQueries();
            createStatisticsQueries();
        } catch (...) {
            destroy();
            throw;
        }
    }

    ~Impl() { destroy(); }

    void destroy() noexcept {
        for (auto* buffer : {&positions,
                             &projectionSettings,
                             &orientations,
                             &harmonics,
                             &projected,
                             &projectedNormals,
                             &keys,
                             &indices,
                             &visibility,
                             &staging,
                             &counter,
                             &counterReadback,
                             &pixelReadback}) {
            destroyBuffer(allocator, *buffer);
        }
        for (auto* pipeline : {&projection,
                               &projectionNormals,
                               &shading,
                               &raster,
                               &rasterDepth,
                               &rasterNormals,
                               &rasterDepthNormals,
                               &compose,
                               &composeFloat}) {
            destroyPipeline(context, *pipeline);
        }
        vkDestroyQueryPool(context.device, queries, nullptr);
        vkDestroyQueryPool(context.device, statisticsQueries, nullptr);
        vkDestroyFence(context.device, fence, nullptr);
        vkDestroyCommandPool(context.device, commands, nullptr);
        vkDestroySampler(context.device, sampler, nullptr);
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

    void createGraphicsPipeline(std::string_view directory,
                                NativePipeline& pipeline,
                                bool withDepth,
                                bool withNormals = false) {
        const std::array types{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                               VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                               VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
        const auto attachmentCount = 1 + std::uint32_t(withDepth) + std::uint32_t(withNormals);
        createNativeLayout(context,
                           pipeline,
                           std::span(types).first(attachmentCount),
                           VK_SHADER_STAGE_VERTEX_BIT,
                           sizeof(GraphicsParameters));
        std::string stem = "native_quad";
        if (withDepth) {
            stem += "_depth";
        }
        if (withNormals) {
            stem += "_normals";
        }
        const auto vertex = loadNativeShader(context, directory, stem + ".vert.spv");
        VkShaderModule fragment = VK_NULL_HANDLE;
        try {
            fragment = loadNativeShader(context, directory, stem + ".frag.spv");
            createGraphicsStages(vertex, fragment, pipeline, withDepth, withNormals);
        } catch (...) {
            vkDestroyShaderModule(context.device, vertex, nullptr);
            vkDestroyShaderModule(context.device, fragment, nullptr);
            throw;
        }
        vkDestroyShaderModule(context.device, vertex, nullptr);
        vkDestroyShaderModule(context.device, fragment, nullptr);
    }

    void createGraphicsStages(VkShaderModule vertex,
                              VkShaderModule fragment,
                              NativePipeline& pipeline,
                              bool withDepth,
                              bool withNormals) {
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
        const std::array attachments{blendState(), blendState(), blendState()};
        auto blending = vkInfo<VkPipelineColorBlendStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
        blending.attachmentCount = 1 + std::uint32_t(withDepth) + std::uint32_t(withNormals);
        blending.pAttachments = attachments.data();
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
        info.layout = pipeline.layout;
        info.renderPass = targets.pass(withDepth, withNormals);
        vkCheck(
            vkCreateGraphicsPipelines(context.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline.handle),
            "vkCreateGraphicsPipelines native");
    }

    void createComposePipeline(std::string_view directory, NativePipeline& pipeline, bool withFloat) {
        const std::array types{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                               VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                               VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                               VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                               VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
        createNativeCompute(context,
                            pipeline,
                            directory,
                            withFloat ? "native_compose_float.comp.spv" : "native_compose.comp.spv",
                            std::span(types).first(withFloat ? 5 : 2),
                            sizeof(ComposeParameters));
    }

    void createPipelines(std::string_view directory) {
        createNativeCompute(
            context, projection, directory, "native_project.comp.spv", 4, sizeof(ProjectParameters));
        createNativeCompute(
            context, shading, directory, "native_shading.comp.spv", 1, sizeof(ProjectParameters));
        createGraphicsPipeline(directory, raster, false);
        createGraphicsPipeline(directory, rasterDepth, true);
        createComposePipeline(directory, compose, false);
        createComposePipeline(directory, composeFloat, true);
    }

    void ensureNormals() {
        if (!targets.hasNormals()) {
            destroyBuffer(allocator, projectedNormals);
            return;
        }
        reserveBuffer(allocator,
                      projectedNormals,
                      targetCount * normalBytes,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      NativeHostAccess::None,
                      counts);
        if (projectionNormals.handle) {
            return;
        }
        try {
            createNativeCompute(context,
                                projectionNormals,
                                shaderDirectory,
                                "native_project_normals.comp.spv",
                                5,
                                sizeof(ProjectParameters));
            createGraphicsPipeline(shaderDirectory, rasterNormals, false, true);
            createGraphicsPipeline(shaderDirectory, rasterDepthNormals, true, true);
        } catch (...) {
            for (auto* pipeline : {&projectionNormals, &rasterNormals, &rasterDepthNormals}) {
                destroyPipeline(context, *pipeline);
            }
            throw;
        }
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

    void createStatisticsQueries() {
        if (!context.pipelineStatisticsEnabled) {
            return;
        }
        auto info = vkInfo<VkQueryPoolCreateInfo>(VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);
        info.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
        info.queryCount = 1;
        info.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
        vkCheck(vkCreateQueryPool(context.device, &info, nullptr, &statisticsQueries),
                "vkCreateQueryPool fragment statistics");
    }

    std::size_t positionStride() const { return positionBytes * (temporal.active() ? 2 : 1); }

    std::size_t harmonicStride() const {
        const auto coefficients = (scene->shRestCoefficients() + 1) * 3;
        return (coefficients * (temporal.degree + 1) + std::size_t(temporal.active())) * sizeof(float);
    }

    bool ensureScene(const FrameState& frame, const GaussianTemporalView& nextTemporal) {
        if (scene != frame.scene) {
            validateScene(*frame.scene);
        }
        const auto nextCount = frame.renderSettings.budgetedCount(frame.scene->count());
        const bool changed =
            scene != frame.scene || targetCount != nextCount || temporal.active() != nextTemporal.active() ||
            temporal.degree != nextTemporal.degree || temporal.period != nextTemporal.period ||
            temporal.times.data() != nextTemporal.times.data() ||
            temporal.variances.data() != nextTemporal.variances.data() ||
            temporal.velocities.data() != nextTemporal.velocities.data() ||
            temporal.harmonics.data() != nextTemporal.harmonics.data();
        const bool timeChanged = temporal.time != nextTemporal.time;
        if (changed) {
            rejectedTile.reset();
            scene = frame.scene;
            targetCount = nextCount;
            uploadedCount = 0;
            ++uploads;
        }
        temporal = nextTemporal;
        if (changed || timeChanged) {
            capture.reset();
        }
        return changed || timeChanged;
    }

    void ensureBuffers() {
        reserveBuffer(allocator,
                      projectionSettings,
                      sizeof(ProjectionSettings),
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      NativeHostAccess::None,
                      counts);
        reserveBuffer(allocator,
                      positions,
                      targetCount * positionStride(),
                      modelBufferUsage,
                      NativeHostAccess::None,
                      counts);
        reserveBuffer(allocator,
                      orientations,
                      targetCount * orientationBytes,
                      modelBufferUsage,
                      NativeHostAccess::None,
                      counts);
        reserveBuffer(allocator,
                      harmonics,
                      targetCount * harmonicStride(),
                      modelBufferUsage,
                      NativeHostAccess::None,
                      counts);
        reserveBuffer(allocator,
                      projected,
                      targetCount * projectedBytes,
                      physicalBufferUsage,
                      NativeHostAccess::None,
                      counts);
        reserveSortStorage();
        reserveTelemetryStorage();
    }

    void reserveSortStorage() {
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
                      visibility,
                      sizeof(NativeVisibilityControl),
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      NativeHostAccess::None,
                      counts);
        sorter.reserve(static_cast<std::uint32_t>(targetCount));
    }

    void reserveTelemetryStorage() {
        reserveBuffer(allocator,
                      counter,
                      projectionCounterBytes,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      NativeHostAccess::None,
                      counts);
        reserveBuffer(allocator,
                      counterReadback,
                      projectionCounterBytes + sizeof(NativeVisibilityControl),
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      NativeHostAccess::Readback,
                      counts);
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

    void packPosition(std::byte* output, std::size_t source) const {
        const std::array<float, 4> position{scene->positions[source * 3],
                                            scene->positions[source * 3 + 1],
                                            scene->positions[source * 3 + 2],
                                            scene->opacityLogits[source]};
        std::memcpy(output, position.data(), positionBytes);
        if (temporal.active()) {
            const std::array<float, 4> motion{temporal.velocities[source * 3],
                                              temporal.velocities[source * 3 + 1],
                                              temporal.velocities[source * 3 + 2],
                                              temporal.times[source]};
            std::memcpy(output + positionBytes, motion.data(), positionBytes);
        }
    }

    void packOrientation(std::byte* output, std::size_t source) const {
        std::memcpy(output, scene->rotations.data() + source * 4, 4 * sizeof(float));
        const std::array<float, 4> scale{scene->logScales[source * 3],
                                         scene->logScales[source * 3 + 1],
                                         scene->logScales[source * 3 + 2],
                                         temporal.active() ? temporal.variances[source] : 0};
        std::memcpy(output + 4 * sizeof(float), scale.data(), 4 * sizeof(float));
    }

    void packHarmonics(std::byte* output, std::size_t source) const {
        const auto baseBytes = (scene->shRestCoefficients() + 1) * 3 * sizeof(float);
        std::memcpy(output, scene->sh0.data() + source * 3, 3 * sizeof(float));
        if (scene->shRestCoefficients()) {
            std::memcpy(output + 3 * sizeof(float),
                        scene->shRest.data() + source * scene->shRestCoefficients() * 3,
                        baseBytes - 3 * sizeof(float));
        }
        if (temporal.active()) {
            const auto temporalBytes = baseBytes * temporal.degree;
            if (temporalBytes) {
                std::memcpy(output + baseBytes,
                            temporal.harmonics.data() + source * temporalBytes / sizeof(float),
                            temporalBytes);
            }
            const auto phase =
                float(std::remainder(double(temporal.times[source]), temporal.period) / temporal.period);
            std::memcpy(output + baseBytes + temporalBytes, &phase, sizeof(phase));
        }
    }

    void packUpload(std::size_t end) {
        const auto points = end - uploadedCount;
        auto* bytes = static_cast<std::byte*>(staging.mapped);
        for (std::size_t i = 0; i < points; ++i) {
            const auto source = sampledGaussian(uploadedCount + i, scene->count(), targetCount);
            packPosition(bytes + i * positionStride(), source);
            packOrientation(bytes + points * positionStride() + i * orientationBytes, source);
            packHarmonics(bytes + points * (positionStride() + orientationBytes) + i * harmonicStride(),
                          source);
        }
    }

    std::size_t uploadEnd(const FrameState& frame) const {
        const auto batch = frame.renderSettings.uploadGaussiansPerFrame;
        return batch ? std::min(targetCount, uploadedCount + batch) : targetCount;
    }

    void reserveUpload(const FrameState& frame) {
        const auto points = uploadEnd(frame) - uploadedCount;
        if (!points) {
            return;
        }
        const auto bytes = points * (positionStride() + orientationBytes + harmonicStride());
        reserveBuffer(
            allocator, staging, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, NativeHostAccess::Upload, counts);
    }

    void upload(const FrameState& frame) {
        const auto end = uploadEnd(frame);
        if (end == uploadedCount) {
            return;
        }
        const auto points = end - uploadedCount;
        const auto bytes = points * (positionStride() + orientationBytes + harmonicStride());
        packUpload(end);
        vkCheck(vmaFlushAllocation(allocator, staging.allocation, 0, bytes),
                "vmaFlushAllocation native upload");
        const std::array<const NativeBuffer*, 3> targets{&positions, &orientations, &harmonics};
        const std::array strides{positionStride(), orientationBytes, harmonicStride()};
        std::size_t offset = 0;
        for (std::size_t i = 0; i < targets.size(); ++i) {
            const VkBufferCopy copy{offset, uploadedCount * strides[i], points * strides[i]};
            vkCmdCopyBuffer(command, staging.handle, targets[i]->handle, 1, &copy);
            offset += points * strides[i];
        }
        uploadedCount = end;
    }

    ProjectParameters parameters(const FrameState& frame, const CameraView& camera, bool diagnostics) const {
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
        if (temporal.active()) {
            result.forward[3] = float(temporal.time);
            result.eye[3] = float(temporal.time - double(result.forward[3]));
            result.viewport[3] = float(std::remainder(temporal.time, temporal.period) / temporal.period);
        }
        result.controls = {static_cast<std::uint32_t>(uploadedCount),
                           scene->shDegree,
                           static_cast<std::uint32_t>(settings.attributeView),
                           std::uint32_t(scene->antialiased) | (std::uint32_t(diagnostics) << 1) |
                               (std::uint32_t(temporal.active()) << 2) | (std::uint32_t(diagnostics) << 3) |
                               (std::uint32_t(frame.renderSettings.separateShading) << 4) |
                               (temporal.degree << 8)};
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

    void project(const FrameState& frame, const CameraView& camera, bool diagnostics) {
        const ProjectionSettings settings{camera.intrinsics,
                                          {camera.depthRange[0],
                                           camera.depthRange[1],
                                           float(camera.projection == CameraProjection::Orthographic),
                                           0}};
        stamp(0);
        vkCmdUpdateBuffer(command, projectionSettings.handle, 0, sizeof(settings), &settings);
        if (diagnostics) {
            vkCmdFillBuffer(command, counter.handle, 0, projectionCounterBytes, 0);
        }
        nativeMemoryBarrier(command,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        const auto* normalBuffer = targets.hasNormals() ? &projectedNormals : nullptr;
        const auto& pipeline = normalBuffer ? projectionNormals : projection;
        bindNativeBuffers(command,
                          pipeline,
                          VK_PIPELINE_BIND_POINT_COMPUTE,
                          {&keys, &indices, &counter, &projectionSettings},
                          normalBuffer);
        const auto projectParameters = parameters(frame, camera, diagnostics);
        vkCmdPushConstants(command,
                           pipeline.layout,
                           VK_SHADER_STAGE_COMPUTE_BIT,
                           0,
                           sizeof(projectParameters),
                           &projectParameters);
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

    void shade(const FrameState& frame, const CameraView& camera, bool diagnostics) {
        if (!frame.renderSettings.separateShading ||
            frame.renderSettings.attributeView != AttributeView::Normal) {
            return;
        }
        bindNativeBuffers(command, shading, VK_PIPELINE_BIND_POINT_COMPUTE, {&projectionSettings});
        const auto settings = parameters(frame, camera, diagnostics);
        vkCmdPushConstants(
            command, shading.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(settings), &settings);
        vkCmdDispatch(command,
                      (static_cast<std::uint32_t>(uploadedCount) + projectGroupSize - 1) / projectGroupSize,
                      1,
                      1);
        nativeMemoryBarrier(command,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            VK_ACCESS_SHADER_WRITE_BIT,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }

    void rasterize(const FrameState& frame, bool statistics) {
        const bool collect = statistics && statisticsQueries;
        if (collect) {
            vkCmdResetQueryPool(command, statisticsQueries, 0, 1);
            vkCmdBeginQuery(command, statisticsQueries, 0, 0);
        }
        nativeMemoryBarrier(command,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            VK_ACCESS_SHADER_WRITE_BIT,
                            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
        const auto* normalBuffer = targets.hasNormals() ? &projectedNormals : nullptr;
        const auto& pipeline = normalBuffer ? (targets.hasDepth() ? rasterDepthNormals : rasterNormals)
                                            : (targets.hasDepth() ? rasterDepth : raster);
        const VkClearValue clear{{{0, 0, 0, 1}}};
        const std::array clears{clear, clear, clear};
        auto pass = vkInfo<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
        pass.renderPass = targets.pass(targets.hasDepth(), targets.hasNormals());
        pass.framebuffer = targets.framebuffer();
        pass.renderArea.extent = {frame.width, frame.height};
        pass.clearValueCount = 1 + std::uint32_t(targets.hasDepth()) + std::uint32_t(targets.hasNormals());
        pass.pClearValues = clears.data();
        vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
        if (targets.hasDepth()) {
            bindNativeBuffers(
                command, pipeline, VK_PIPELINE_BIND_POINT_GRAPHICS, {&indices, &keys}, normalBuffer);
        } else {
            bindNativeBuffers(command, pipeline, VK_PIPELINE_BIND_POINT_GRAPHICS, {&indices}, normalBuffer);
        }
        const GraphicsParameters settings{bufferAddress(context, projected),
                                          {float(frame.width), float(frame.height)}};
        vkCmdPushConstants(
            command, pipeline.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(settings), &settings);
        const VkViewport viewport{0, 0, float(frame.width), float(frame.height), 0, 1};
        const VkRect2D scissor{{0, 0}, {frame.width, frame.height}};
        vkCmdSetViewport(command, 0, 1, &viewport);
        vkCmdSetScissor(command, 0, 1, &scissor);
        if (tiler) {
            vkCmdDrawIndirect(command,
                              tiler->fallbackArguments().handle,
                              NativeTileRaster::fallbackOffset,
                              1,
                              sizeof(VkDrawIndirectCommand));
        } else {
            vkCmdDrawIndirect(command,
                              visibility.handle,
                              offsetof(NativeVisibilityControl, draw),
                              1,
                              sizeof(VkDrawIndirectCommand));
        }
        vkCmdEndRenderPass(command);
        if (collect) {
            vkCmdEndQuery(command, statisticsQueries, 0);
        }
        if (tiler) {
            tiler->recordRaster(command, frame, rasterInput(), targets, statistics);
        }
        stamp(5);
    }

    void makeWritable(VkImage image, bool initialized) {
        auto barrier =
            imageBarrier(image,
                         initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_GENERAL);
        barrier.srcAccessMask = initialized ? VK_ACCESS_SHADER_READ_BIT : 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        nativeImageBarrier(command,
                           initialized ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT
                                       : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           barrier);
    }

    void bindComposeImages(const NativePipeline& pipeline, VkImageView display, bool withFloat) {
        const auto& fallback = targets.colorAlpha.resource.image ? targets.colorAlpha : targets.depth;
        const auto& color = targets.colorAlpha.resource.image ? targets.colorAlpha : fallback;
        const auto& depth = targets.depth.resource.image ? targets.depth : fallback;
        const auto depthSource =
            targets.hasDepth() ? targets.depthAccum.resource.view : targets.colorAccum.resource.view;
        const std::array<VkDescriptorImageInfo, 5> infos{
            {{sampler, targets.colorAccum.resource.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
             {VK_NULL_HANDLE, display, VK_IMAGE_LAYOUT_GENERAL},
             {VK_NULL_HANDLE, color.resource.view, VK_IMAGE_LAYOUT_GENERAL},
             {sampler, depthSource, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
             {VK_NULL_HANDLE, depth.resource.view, VK_IMAGE_LAYOUT_GENERAL}}};
        std::array<VkWriteDescriptorSet, 5> writes{};
        const std::uint32_t count = withFloat ? 5 : 2;
        for (std::uint32_t i = 0; i < count; ++i) {
            writes[i] = vkInfo<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = (i == 0 || i == 3) ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                          : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[i].pImageInfo = &infos[i];
        }
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.handle);
        vkCmdPushDescriptorSetKHR(
            command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.layout, 0, count, writes.data());
    }

    void composeImage(const FrameState& frame, VkImage image, VkImageView view, bool initialized) {
        makeWritable(image, initialized);
        RenderOutput outputs = RenderOutput::Display;
        for (auto* target : {&targets.colorAlpha, &targets.depth}) {
            if (target->resource.image) {
                makeWritable(target->resource.image, target->hasContent);
                outputs = outputs |
                          (target == &targets.depth ? RenderOutput::Depth : RenderOutput::FloatColorAlpha);
            }
        }
        const bool withFloat = outputs != RenderOutput::Display;
        const auto& pipeline = withFloat ? composeFloat : compose;
        bindComposeImages(pipeline, view, withFloat);
        ComposeParameters settings{};
        std::copy(frame.renderSettings.background.begin(),
                  frame.renderSettings.background.end(),
                  settings.background.begin());
        settings.controls[0] = std::uint32_t(outputs);
        vkCmdPushConstants(
            command, pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(settings), &settings);
        vkCmdDispatch(command,
                      (frame.width + composeGroupSize - 1) / composeGroupSize,
                      (frame.height + composeGroupSize - 1) / composeGroupSize,
                      1);
        stamp(6);
    }

    VkImage captureImage(RenderOutput output, VkImage display) const {
        if (output == RenderOutput::Display) {
            return display;
        }
        if (output == RenderOutput::Normals) {
            return targets.normals.resource.image;
        }
        return output == RenderOutput::Depth ? targets.depth.resource.image
                                             : targets.colorAlpha.resource.image;
    }

    void recordCapture(const FrameState& frame, VkImage display, const NativeCapturePlan& plan) {
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
                command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pixelReadback.handle, 1, &copy);
        }
    }

    void makeReadable(VkImage image, bool captured) {
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

    void finishRecording(VkImage image, bool sampled, RenderOutput captured) {
        if (sampled) {
            nativeMemoryBarrier(command,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                VK_ACCESS_SHADER_WRITE_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_ACCESS_TRANSFER_READ_BIT);
            if (std::uint32_t(captured)) {
                const VkBufferCopy copy{0, 0, projectionCounterBytes};
                vkCmdCopyBuffer(command, counter.handle, counterReadback.handle, 1, &copy);
            }
            const VkBufferCopy visibilityCopy{0, projectionCounterBytes, sizeof(NativeVisibilityControl)};
            vkCmdCopyBuffer(command, visibility.handle, counterReadback.handle, 1, &visibilityCopy);
            if (tiler) {
                tiler->recordTelemetry(command);
            }
        }
        if (sampled || std::uint32_t(captured)) {
            nativeMemoryBarrier(command,
                                VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_ACCESS_TRANSFER_WRITE_BIT,
                                VK_PIPELINE_STAGE_HOST_BIT,
                                VK_ACCESS_HOST_READ_BIT);
        }
        makeReadable(image, hasOutput(captured, RenderOutput::Display));
        for (auto* target : {&targets.colorAlpha, &targets.depth}) {
            if (!target->resource.image) {
                continue;
            }
            const auto plane = target == &targets.depth ? RenderOutput::Depth : RenderOutput::FloatColorAlpha;
            makeReadable(target->resource.image, hasOutput(captured, plane));
            target->resource.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        if (targets.hasNormals()) {
            if (hasOutput(captured, RenderOutput::Normals)) {
                makeReadable(targets.normals.resource.image, true);
            }
            targets.normals.resource.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
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

    void readTicks(std::uint32_t first, std::span<std::uint64_t> values) {
        vkCheck(vkGetQueryPoolResults(context.device,
                                      queries,
                                      first,
                                      static_cast<std::uint32_t>(values.size()),
                                      values.size_bytes(),
                                      values.data(),
                                      sizeof(values[0]),
                                      VK_QUERY_RESULT_64_BIT),
                "vkGetQueryPoolResults native");
    }

    void readTileTimings(BackendStats& stats,
                         const std::array<std::uint64_t, frameTimestampCount>& frame,
                         bool captured) {
        if (!tiler || !captured) {
            return;
        }
        std::array<std::uint64_t, tileTimestampCount> phases{};
        readTicks(frameTimestampCount, phases);
        const std::array ticks{frame[3], phases[0], phases[1], phases[2], phases[3], frame[4]};
        const auto interval = [&](std::size_t i) {
            return double((ticks[i + 1] - ticks[i]) & timestampMask) * timestampPeriod / 1'000'000.;
        };
        stats.tileStageTimings =
            TileStageTimings{interval(0), interval(1), interval(2), interval(3), interval(4)};
    }

    void readTimestamps(const FrameState& state, FrameReport& report, bool captured) {
        auto& frame = report.frame;
        report.backend.tileStageTimings.reset();
        frame.projectionGpuMs.reset();
        frame.shadingGpuMs.reset();
        frame.sortingGpuMs.reset();
        frame.binningGpuMs.reset();
        frame.rasterGpuMs.reset();
        frame.composeGpuMs.reset();
        frame.gpuRenderMs.reset();
        if (!queries) {
            return;
        }
        std::array<std::uint64_t, frameTimestampCount> values{};
        readTicks(0, values);
        readTileTimings(report.backend, values, captured);
        const auto interval = [&](std::size_t i) {
            return double((values[i + 1] - values[i]) & timestampMask) * timestampPeriod / 1'000'000.;
        };
        frame.projectionGpuMs = interval(0);
        if (state.renderSettings.separateShading &&
            state.renderSettings.attributeView == AttributeView::Normal) {
            frame.shadingGpuMs = interval(1);
        }
        frame.sortingGpuMs = interval(2);
        if (tiler) {
            frame.binningGpuMs = interval(3);
        }
        frame.rasterGpuMs = interval(4);
        frame.composeGpuMs = interval(5);
        frame.gpuRenderMs = interval(0) + interval(1) + interval(2) + interval(3) + interval(4);
    }

    void readProjectionCounters(BackendStats& stats, bool diagnostics) {
        vkCheck(vmaInvalidateAllocation(allocator, counterReadback.allocation, 0, counterReadback.capacity),
                "vmaInvalidateAllocation native counters");
        NativeVisibilityControl compacted{};
        std::memcpy(&compacted,
                    static_cast<const std::byte*>(counterReadback.mapped) + projectionCounterBytes,
                    sizeof(compacted));
        stats.visibleGaussians = compacted.sort.count;
        stats.sortedGaussians = compacted.sort.count;
        stats.projectionDiagnostics.reset();
        if (diagnostics) {
            std::array<std::uint32_t, projectionCounterWords> values{};
            std::memcpy(values.data(), counterReadback.mapped, projectionCounterBytes);
            stats.independentlyProjectedGaussians = values[0];
            stats.projectionDiagnostics = decodeDiagnostics(values);
        }
    }

    void readStatistics(BackendStats& stats, bool captured) {
        stats.fragmentShaderInvocations.reset();
        if (!captured || !statisticsQueries) {
            return;
        }
        std::uint64_t invocations = 0;
        vkCheck(vkGetQueryPoolResults(context.device,
                                      statisticsQueries,
                                      0,
                                      1,
                                      sizeof(invocations),
                                      &invocations,
                                      sizeof(invocations),
                                      VK_QUERY_RESULT_64_BIT),
                "read fragment statistics");
        stats.fragmentShaderInvocations = invocations;
    }

    void readResults(const FrameState& frame,
                     FrameReport& report,
                     bool sample,
                     const NativeCapturePlan& plan) {
        const auto start = Clock::now();
        readTimestamps(frame, report, plan.count != 0);
        readStatistics(report.backend, plan.count != 0);
        report.backend.independentlyProjectedGaussians.reset();
        report.backend.projectionDiagnostics.reset();
        if (sample) {
            readProjectionCounters(report.backend, plan.count != 0);
            if (tiler) {
                tiler->readTelemetry(plan.count != 0);
            }
            report.backend.telemetryFrameIndex = report.frame.frameIndex + 1;
            lastCounterSample = Clock::now();
        }
        report.backend.telemetryAgeMs = milliseconds(lastCounterSample);
        report.frame.queryReadbackCpuMs = milliseconds(start);
        if (plan.count) {
            vkCheck(vmaInvalidateAllocation(allocator, pixelReadback.allocation, 0, plan.bytes),
                    "vmaInvalidateAllocation native pixels");
            capture = nativeReadCapture(frame, plan, pixelReadback.mapped);
            pixelReadbacks += plan.count;
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
        stats.tileAllocationFallback = rejectedTile.has_value();
        stats.rasterizer = tiler ? "native-vulkan-hybrid-tiles" : "native-vulkan-ewa-quads";
        if (tiler) {
            tiler->fillStats(stats);
            stats.tileBinningWidth = state.renderSettings.tileBinningWidth;
        } else {
            stats.tilePairsReserved.reset();
            stats.cooperativeGaussians.reset();
            stats.tilePairsDemandLowerBound.reset();
            stats.tilePairOverflow.reset();
            stats.tiledRasterization.reset();
            stats.tilePairCapacity = 0;
            stats.tileBinningWidth = 0;
            stats.tileDiagnostics.reset();
        }
        stats.gsBufferAllocations = counts.allocations;
        stats.gsBufferReuses = counts.reuses;
        stats.gsGrowths = counts.growths;
        stats.modelBytes = positions.capacity + orientations.capacity + harmonics.capacity + staging.capacity;
        stats.preprocessingBytes =
            projected.capacity + projectedNormals.capacity + projectionSettings.capacity;
        stats.sortingBytes = keys.capacity + indices.capacity + visibility.capacity + sorter.bytes() +
                             (tiler ? tiler->workspaceBytes() : 0);
        stats.pixelBytes = targets.bytes();
        stats.telemetryBytes = counter.capacity + counterReadback.capacity + pixelReadback.capacity +
                               (tiler ? tiler->telemetryBytes() : 0);
        stats.gsReservedBytes = stats.modelBytes + stats.preprocessingBytes + stats.sortingBytes +
                                stats.pixelBytes + stats.telemetryBytes;
    }

    NativeRasterInput rasterInput() const {
        return {projected,
                keys,
                indices,
                static_cast<std::uint32_t>(uploadedCount),
                &visibility,
                targets.hasNormals() ? &projectedNormals : nullptr};
    }

    bool ensureTiler(const FrameState& frame) {
        const auto mode = frame.renderSettings.rasterMode;
        const bool wanted = mode == RasterMode::TiledCompute;
        if (!wanted) {
            tiler.reset();
            rejectedTile.reset();
            return false;
        }
        const std::array configuration{targetCount,
                                       std::size_t(frame.width),
                                       std::size_t(frame.height),
                                       std::size_t(frame.renderSettings.tilePairCapacity),
                                       targets.bytes() + projectedNormals.capacity};
        if (rejectedTile == configuration) {
            return false;
        }
        const bool created = !tiler;
        try {
            if (created) {
                tiler = std::make_unique<NativeTileRaster>(context, allocator, counts, shaderDirectory);
            }
            tiler->reserve(frame, targetCount, targets.hasNormals());
        } catch (const VulkanError& error) {
            if (!capacityError(error)) {
                throw;
            }
            tiler.reset();
            rejectedTile = configuration;
            return false;
        }
        return created;
    }

    void reserveCapture(const FrameState& frame, const NativeCapturePlan& plan) {
        if (!plan.count) {
            return;
        }
        if (pixelReadback.capacity > frame.captureBudgetBytes) {
            destroyBuffer(allocator, pixelReadback);
        }
        reserveBuffer(allocator,
                      pixelReadback,
                      plan.bytes,
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      NativeHostAccess::Readback,
                      counts);
    }

    void reserveMandatoryStorage(const FrameState& frame,
                                 const NativeCapturePlan& capturePlan,
                                 FrameReport& report) {
        ensureBuffers();
        targets.ensure(frame,
                       frame.renderSettings.outputs |
                           (capturePlan.count ? frame.captureOutputs : RenderOutput(0)),
                       report);
        ensureNormals();
        reserveUpload(frame);
        reserveCapture(frame, capturePlan);
    }

    void reserveFrameStorage(const FrameState& frame,
                             const NativeCapturePlan& capturePlan,
                             FrameReport& report) {
        try {
            reserveMandatoryStorage(frame, capturePlan, report);
        } catch (const VulkanError& error) {
            if (!tiler || !capacityError(error)) {
                throw;
            }
            // A retained optional tile pool must not starve a larger mandatory model or upload.
            tiler.reset();
            reserveMandatoryStorage(frame, capturePlan, report);
        }
    }

    PreparedFrame prepareFrame(const FrameState& frame,
                               const GaussianTemporalView& nextTemporal,
                               FrameReport& report) {
        validateSettings(frame.renderSettings);
        PreparedFrame prepared{};
        prepared.camera = cameraView(frame.camera, frame.width, frame.height);
        const bool changed = ensureScene(frame, nextTemporal);
        const bool pending =
            frame.captureRequestId && (!capture || capture->requestId != frame.captureRequestId ||
                                       capture->outputs != frame.captureOutputs);
        if (pending) {
            prepared.capture = nativeCapturePlan(frame);
        }
        const bool complete = uploadEnd(frame) == targetCount;
        if (!complete) {
            prepared.capture = {};
        }
        prepared.sample = changed || pending || (uploadedCount < targetCount && complete) ||
                          Clock::now() - lastCounterSample >= counterInterval;
        reserveFrameStorage(frame, prepared.capture, report);
        if (ensureTiler(frame)) {
            prepared.sample = true;
        }
        return prepared;
    }

    void render(const FrameState& frame,
                const GaussianTemporalView& nextTemporal,
                VkImage image,
                VkImageView view,
                bool initialized,
                FrameReport& report) {
        const auto prepared = prepareFrame(frame, nextTemporal, report);
        const auto previousUpload = uploadedCount;
        try {
            begin();
            upload(frame);
            project(frame, prepared.camera, prepared.capture.count != 0);
            shade(frame, prepared.camera, prepared.capture.count != 0);
            stamp(2);
            sorter.record(command,
                          static_cast<std::uint32_t>(uploadedCount),
                          keys,
                          indices,
                          &visibility,
                          positiveDepthKeyBits);
            stamp(3);
            if (tiler) {
                tiler->recordBinning(
                    command,
                    frame,
                    rasterInput(),
                    {prepared.capture.count ? queries : VK_NULL_HANDLE, frameTimestampCount});
            }
            stamp(4);
            rasterize(frame, prepared.capture.count != 0);
            composeImage(frame, image, view, initialized);
            recordCapture(frame, image, prepared.capture);
            finishRecording(
                image, prepared.sample, prepared.capture.count ? frame.captureOutputs : RenderOutput(0));
            report.frame.backendWaitMs = submitAndWait();
        } catch (...) {
            uploadedCount = previousUpload;
            throw;
        }
        if (temporal.active() && previousUpload != uploadedCount && uploadedCount == targetCount) {
            destroyBuffer(allocator, staging);
        }
        for (auto* target : {&targets.colorAlpha, &targets.depth, &targets.normals}) {
            target->hasContent = target->resource.image != VK_NULL_HANDLE;
        }
        readResults(frame, report, prepared.sample, prepared.capture);
        fillReport(frame, report);
    }
};

NativeGaussianRenderer::NativeGaussianRenderer(DeviceContext context,
                                               VmaAllocator allocator,
                                               std::string directory)
    : impl_(std::make_unique<Impl>(context, allocator, directory)) {}

NativeGaussianRenderer::~NativeGaussianRenderer() = default;

VulkanRenderImages NativeGaussianRenderer::outputImages() const { return impl_->targets.outputImages(); }

void NativeGaussianRenderer::render(const FrameState& frame,
                                    VkImage image,
                                    VkImageView view,
                                    bool initialized,
                                    FrameReport& report) {
    impl_->render(frame, {}, image, view, initialized, report);
}

void NativeGaussianRenderer::renderTemporal(const FrameState& frame,
                                            const GaussianTemporalView& temporal,
                                            VkImage image,
                                            VkImageView view,
                                            bool initialized,
                                            FrameReport& report) {
    const auto count = frame.scene ? frame.scene->count() : 0;
    const auto coefficients = frame.scene ? (frame.scene->shRestCoefficients() + 1) * 3 : 0;
    if (!count || temporal.times.size() != count || temporal.variances.size() != count ||
        temporal.velocities.size() != count * 3 ||
        temporal.harmonics.size() != count * coefficients * temporal.degree || temporal.degree > 2 ||
        !std::isfinite(temporal.time) || !std::isfinite(temporal.period) || temporal.period <= 0) {
        throw std::invalid_argument("Invalid Gaussian temporal frame");
    }
    impl_->render(frame, temporal, image, view, initialized, report);
}
}
