// This adapter links the temporary GPL-3.0-or-later LichtFeld backend.
// See third_party/lichtfeld_vulkan/UPSTREAM.md and docs/dependencies.md.
#include "static_gs_renderer.hpp"
#include <gs_renderer.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace orbelio {
namespace {
using Clock = std::chrono::steady_clock;

constexpr std::size_t countersBufferSizeBytes = 12;
constexpr std::size_t visibleCountFieldOffset = 8;
constexpr std::size_t tileTotalsReadbackBytes = 8;
constexpr std::size_t tileTotalsClearMargin = 2;
constexpr std::size_t commandUpdateChunkBytes = 65536;
constexpr std::uint32_t statisticsWorkgroupSize = 256;
constexpr std::uint32_t composeWorkgroupSize = 8;
constexpr std::uint32_t statisticsPushBytes = 4;
constexpr std::uint32_t composePushBytes = 8;
constexpr std::size_t sortBitsMinimum = 1;
constexpr std::size_t targetBytesPerPixel = 4;
constexpr int timestampQueryCount = 2;
constexpr auto telemetrySampleInterval = std::chrono::milliseconds(250);
constexpr double fullTurnDegrees = 360.;

// Only the legacy full-frame path plus wave partitioning; no training/HiGS pipelines.
constexpr std::array<std::string_view, 32> rasterizerShaders = {"projection_forward",
                                                                "projection_forward_3dgut",
                                                                "selection_mask",
                                                                "selection_polygon_rasterize",
                                                                "generate_keys_wave",
                                                                "compute_tile_ranges",
                                                                "compute_tile_ranges_and_batch_counts",
                                                                "rasterize_forward",
                                                                "rasterize_forward_3dgut",
                                                                "rasterize_forward_plain",
                                                                "rasterize_forward_3dgut_plain",
                                                                "rasterize_forward_light",
                                                                "rasterize_forward_light_plain",
                                                                "rasterize_forward_batches",
                                                                "rasterize_forward_batches_plain",
                                                                "tile_batch_descriptors",
                                                                "compose_tile_batches",
                                                                "compose_tile_batches_plain",
                                                                "cumsum_single_pass",
                                                                "cumsum_block_scan",
                                                                "cumsum_scan_block_sums",
                                                                "cumsum_add_block_offsets",
                                                                "radix_histogram_clear",
                                                                "expected_depth_finalize",
                                                                "apply_depth_ordering",
                                                                "visible_flags",
                                                                "prepare_visible_sort",
                                                                "prepare_tile_sort",
                                                                "compact_visible_primitives",
                                                                "lod_map_indices",
                                                                "lod_select_threshold",
                                                                "wave_partition"};
constexpr std::array<std::string_view, 3> radixSortShaders = {"upsweep_indirect",
                                                              "spine_indirect",
                                                              "downsweep_indirect"};

std::vector<std::uint32_t> readSpv(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error(std::string("Missing SPIR-V: ") + path);
    }
    const auto bytes = file.tellg();
    if (bytes <= 0 || bytes % 4) {
        throw std::runtime_error(std::string("Invalid SPIR-V: ") + path);
    }
    std::vector<std::uint32_t> code(static_cast<std::size_t>(bytes) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), bytes);
    if (!file) {
        throw std::runtime_error(std::string("Incomplete SPIR-V: ") + path);
    }
    return code;
}

VkPipeline createComputePipeline(VkDevice device, VkPipelineLayout layout, const char* path) {
    const auto code = readSpv(path);
    auto moduleInfo = vkInfo<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    moduleInfo.codeSize = code.size() * sizeof(std::uint32_t);
    moduleInfo.pCode = code.data();
    VkShaderModule shader = VK_NULL_HANDLE;
    vkCheck(vkCreateShaderModule(device, &moduleInfo, nullptr, &shader), "create compute shader");

    auto stage = vkInfo<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shader;
    stage.pName = "main";

    auto pipelineInfo = vkInfo<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
    pipelineInfo.stage = stage;
    pipelineInfo.layout = layout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const auto result =
        vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
    vkDestroyShaderModule(device, shader, nullptr);
    vkCheck(result, "create compute pipeline");
    return pipeline;
}

// One end of a target-image layout transition: layout, the stage that owns it, and the access it grants.
struct TargetState {
    VkImageLayout layout;
    VkPipelineStageFlags stage;
    VkAccessFlags access;
};

constexpr TargetState undefinedTarget() {
    return {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0};
}

constexpr TargetState computeTarget() {
    return {VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT};
}

constexpr TargetState copySourceTarget() {
    return {
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT};
}

constexpr TargetState qtReadableTarget() {
    return {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT};
}

void transitionTarget(VkCommandBuffer command, VkImage image, TargetState from, TargetState to) {
    auto barrier = vkInfo<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
    barrier.srcAccessMask = from.access;
    barrier.dstAccessMask = to.access;
    barrier.oldLayout = from.layout;
    barrier.newLayout = to.layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(command, from.stage, to.stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void releaseToHost(VkCommandBuffer command, VkBuffer buffer, VkDeviceSize size) {
    auto barrier = vkInfo<VkBufferMemoryBarrier>(VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer;
    barrier.size = size;
    vkCmdPipelineBarrier(command,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT,
                         0,
                         0,
                         nullptr,
                         1,
                         &barrier,
                         0,
                         nullptr);
}

VkWriteDescriptorSet storageBufferWrite(std::uint32_t binding, const VkDescriptorBufferInfo& info) {
    auto write = vkInfo<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &info;
    return write;
}

VkWriteDescriptorSet storageImageWrite(std::uint32_t binding, const VkDescriptorImageInfo& info) {
    auto write = vkInfo<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = &info;
    return write;
}

VkDescriptorSetLayout createPushDescriptorLayout(VkDevice device,
                                                 std::span<const VkDescriptorSetLayoutBinding> bindings) {
    auto info = vkInfo<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    info.bindingCount = static_cast<std::uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    vkCheck(vkCreateDescriptorSetLayout(device, &info, nullptr, &layout), "create push descriptors");
    return layout;
}

VkPipelineLayout createPipelineLayout(VkDevice device,
                                      VkDescriptorSetLayout setLayout,
                                      VkPushConstantRange range) {
    auto info = vkInfo<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    info.setLayoutCount = 1;
    info.pSetLayouts = &setLayout;
    info.pushConstantRangeCount = range.size ? 1u : 0u;
    info.pPushConstantRanges = &range;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    vkCheck(vkCreatePipelineLayout(device, &info, nullptr, &layout), "create pipeline layout");
    return layout;
}

VmaAllocationCreateInfo hostVisibleAllocation() {
    VmaAllocationCreateInfo info{};
    info.usage = VMA_MEMORY_USAGE_AUTO;
    info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    return info;
}
} // namespace

struct StaticGsRenderer::Impl {
    // Frame values shared between the recording steps and the statistics fill.
    struct CameraFrame {
        // Upstream reads fields this adapter never assigns; it must stay zero-initialised.
        VulkanGSRendererUniforms uniforms{};
        double orbitDegrees = 0.;
    };

    struct RasterScope {
        std::size_t references = 0;
        std::size_t waves = 0;
        std::uint32_t tiles = 0;
    };

    DeviceContext context;
    VmaAllocator allocator;
    VulkanGSRenderer renderer;
    VulkanGSPipelineBuffers buffers;
    Buffer<float> dummy;
    Buffer<float> overlays;
    Buffer<std::uint32_t> tileTotals;

    VkDescriptorSetLayout statsDescriptors = VK_NULL_HANDLE;
    VkPipelineLayout statsLayout = VK_NULL_HANDLE;
    VkPipeline statsPipeline = VK_NULL_HANDLE;
    VkQueryPool composeQueries = VK_NULL_HANDLE;
    double timestampPeriod = 0;
    std::uint32_t timestampBits = 0;
    std::uint32_t currentTiles = 0;

    VkBuffer counters = VK_NULL_HANDLE;
    VmaAllocation countersAllocation = VK_NULL_HANDLE;
    void* counterMap = nullptr;

    std::uint64_t growths = 0;
    std::size_t lastCapacity = 0;
    std::uint64_t lastCensusAllocations = 0;
    bool sampleCounters = true;
    Clock::time_point lastCounterSample;

    std::shared_ptr<const StaticScene> scene;

    VkDescriptorSetLayout descriptors = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline compose = VK_NULL_HANDLE;

    VkBuffer readback = VK_NULL_HANDLE;
    VmaAllocation readbackAllocation = VK_NULL_HANDLE;
    std::size_t readbackCapacity = 0;
    void* mapped = nullptr;

    std::shared_ptr<const FrameCapture> capture;
    std::uint64_t uploads = 0;

    double gpuMs = 0;
    double backendWaitMs = 0;
    double queryReadbackCpuMs = 0;
    double projectionMs = 0;
    double sortingMs = 0;
    double rasterMs = 0;

    Impl(DeviceContext c, VmaAllocator a) : context(c), allocator(a) {}

    void initialize() {
        loadShaders();
        configureBackend();
        detectTimestampSupport();
        createStatisticsPipeline();
        installTileStatisticsCallback();
        createComposePipeline();
    }

    ~Impl() {
        vkDeviceWaitIdle(context.device);
        renderer.cancelCommandBatch();
        renderer.cleanupBuffers(buffers);
        renderer.destroyBuffer(dummy.deviceBuffer);
        renderer.destroyBuffer(overlays.deviceBuffer);
        renderer.destroyBuffer(tileTotals.deviceBuffer);
        if (composeQueries) {
            vkDestroyQueryPool(context.device, composeQueries, nullptr);
        }
        if (statsPipeline) {
            vkDestroyPipeline(context.device, statsPipeline, nullptr);
        }
        if (statsLayout) {
            vkDestroyPipelineLayout(context.device, statsLayout, nullptr);
        }
        if (statsDescriptors) {
            vkDestroyDescriptorSetLayout(context.device, statsDescriptors, nullptr);
        }
        if (counters) {
            vmaDestroyBuffer(allocator, counters, countersAllocation);
        }
        if (readback) {
            vmaDestroyBuffer(allocator, readback, readbackAllocation);
        }
        if (compose) {
            vkDestroyPipeline(context.device, compose, nullptr);
        }
        if (layout) {
            vkDestroyPipelineLayout(context.device, layout, nullptr);
        }
        if (descriptors) {
            vkDestroyDescriptorSetLayout(context.device, descriptors, nullptr);
        }
    }

    [[nodiscard]] std::optional<double> measured(double ms) const {
        // Timestamps are advertised per queue family; without them the metric is unavailable, never zero.
        return timestampBits ? std::optional<double>(ms) : std::nullopt;
    }

    void loadShaders() {
        const auto directory = std::string(LFS_VULKAN_RASTERIZER_DEV_SPV_DIR);
        std::map<std::string, std::string> paths;
        for (const auto name : rasterizerShaders) {
            paths[std::string(name)] = directory + "generated/" + std::string(name) + ".spv";
        }
        for (const auto name : radixSortShaders) {
            paths["radix_sort/" + std::string(name)] = directory + "radix_sort/" + std::string(name) + ".spv";
        }
        renderer.initializeExternal(paths,
                                    context.instance,
                                    context.physicalDevice,
                                    context.device,
                                    context.queue,
                                    context.queueFamily,
                                    allocator);
    }

    void configureBackend() {
        renderer.assignBufferLabels(buffers);
        renderer.setDepthCapture(true); // Plain per-pixel path, complete output at every pixel.
        attachGpuTimers();
        attachCpuTimers();
    }

    void attachGpuTimers() {
        renderer.addTimerCallback([this](const auto& stages) {
            // Top-level stages only: nested radix/cumsum timers must not be counted twice.
            // Upstream callback is indexed by TrainStage; each pair is (sample count, seconds).
            for (std::size_t stage = 0; stage < stages.size(); ++stage) {
                if (!stages[stage].first) {
                    continue;
                }
                const double ms = stages[stage].second * 1000.;
                if (stage == PerfTimer::ProjectionForward) {
                    projectionMs += ms;
                }
                if (stage == PerfTimer::SortPrimitivesByDepth) {
                    sortingMs += ms;
                }
                if (stage == PerfTimer::RasterizeForward) {
                    rasterMs += ms;
                }
                if (stage == PerfTimer::ProjectionForward || stage == PerfTimer::SortPrimitivesByDepth ||
                    stage == PerfTimer::ApplyDepthOrdering ||
                    stage == PerfTimer::CalculateIndexBufferOffset || stage == PerfTimer::RasterizeForward) {
                    gpuMs += ms;
                }
            }
        });
    }

    void attachCpuTimers() {
        renderer.setCpuTimerCallback([this](std::string_view name, double ms) {
            if (name == "vksplat.command_batch.wait_fence" || name == "vksplat.command_batch.wait_pending" ||
                name == "vksplat.command_batch.wait_idle") {
                backendWaitMs += ms;
            }
            if (name == "vksplat.command_batch.query_results") {
                queryReadbackCpuMs += ms;
            }
        });
    }

    void detectTimestampSupport() {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(context.physicalDevice, &properties);
        timestampPeriod = properties.limits.timestampPeriod;

        std::uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(context.physicalDevice, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(context.physicalDevice, &familyCount, families.data());
        timestampBits = families.at(context.queueFamily).timestampValidBits;
        if (!timestampBits) {
            return;
        }

        auto queryInfo = vkInfo<VkQueryPoolCreateInfo>(VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);
        queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        queryInfo.queryCount = timestampQueryCount;
        vkCheck(vkCreateQueryPool(context.device, &queryInfo, nullptr, &composeQueries),
                "compose timestamp pool");
    }

    void createStatisticsPipeline() {
        auto bufferInfo = vkInfo<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
        bufferInfo.size = countersBufferSizeBytes;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        auto allocation = hostVisibleAllocation();
        VmaAllocationInfo allocated{};
        vkCheck(
            vmaCreateBuffer(allocator, &bufferInfo, &allocation, &counters, &countersAllocation, &allocated),
            "statistics readback");
        counterMap = allocated.pMappedData;

        const std::array<VkDescriptorSetLayoutBinding, 3> bindings{
            VkDescriptorSetLayoutBinding{
                0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{
                1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{
                2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        statsDescriptors = createPushDescriptorLayout(context.device, bindings);
        statsLayout =
            createPipelineLayout(context.device,
                                 statsDescriptors,
                                 VkPushConstantRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, statisticsPushBytes});
        statsPipeline = createComputePipeline(context.device, statsLayout, ORBELIO_STATS_SPV);
    }

    // Host-side statistics helper: reduces per-tile reference counts into one max + total.
    void dispatchTileStatistics(const _VulkanBuffer& ranges, const _VulkanBuffer& count) {
        if (!sampleCounters) {
            return;
        }
        using namespace lfs::rendering::vulkan;
        const DeclaredAccess access[] = {{&ranges, BufferUse::ComputeRead},
                                         {&count, BufferUse::ComputeRead},
                                         {&tileTotals.deviceBuffer, BufferUse::ComputeReadWrite}};
        renderer.planTransfer(access);

        const std::array<const _VulkanBuffer*, 3> sources{&ranges, &count, &tileTotals.deviceBuffer};
        std::array<VkDescriptorBufferInfo, sources.size()> infos{};
        std::array<VkWriteDescriptorSet, sources.size()> writes{};
        for (std::size_t i = 0; i < sources.size(); ++i) {
            infos[i] = {sources[i]->buffer, sources[i]->offset, sources[i]->size};
            writes[i] = storageBufferWrite(static_cast<std::uint32_t>(i), infos[i]);
        }

        auto command = renderer.activeCommandBuffer();
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, statsPipeline);
        vkCmdPushDescriptorSetKHR(command,
                                  VK_PIPELINE_BIND_POINT_COMPUTE,
                                  statsLayout,
                                  0,
                                  static_cast<std::uint32_t>(writes.size()),
                                  writes.data());
        vkCmdPushConstants(
            command, statsLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, statisticsPushBytes, &currentTiles);
        vkCmdDispatch(command, (currentTiles + statisticsWorkgroupSize - 1) / statisticsWorkgroupSize, 1, 1);
    }

    void installTileStatisticsCallback() {
        renderer.tileStatsCallback = [this](const _VulkanBuffer& ranges, const _VulkanBuffer& count) {
            dispatchTileStatistics(ranges, count);
        };
    }

    void createComposePipeline() {
        const std::array<VkDescriptorSetLayoutBinding, 2> bindings{
            VkDescriptorSetLayoutBinding{
                0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{
                1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        descriptors = createPushDescriptorLayout(context.device, bindings);
        layout = createPipelineLayout(context.device,
                                      descriptors,
                                      VkPushConstantRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, composePushBytes});
        compose = createComputePipeline(context.device, layout, ORBELIO_COMPOSE_SPV);
    }

    void upload(Buffer<float>& destination, const std::vector<float>& source) {
        auto& target = renderer.resizeDeviceBuffer(destination, source.size());
        const lfs::rendering::vulkan::DeclaredAccess access{&target,
                                                            lfs::rendering::vulkan::BufferUse::TransferWrite};
        renderer.planTransfer(std::span{&access, 1});
        const std::size_t bytes = source.size() * sizeof(float);
        // vkCmdUpdateBuffer snapshots CPU bytes into reusable command buffers;
        // no per-frame staging allocation.
        for (std::size_t offset = 0; offset < bytes; offset += commandUpdateChunkBytes) {
            const auto chunk = std::min<std::size_t>(commandUpdateChunkBytes, bytes - offset);
            vkCmdUpdateBuffer(renderer.activeCommandBuffer(),
                              target.buffer,
                              target.offset + offset,
                              chunk,
                              reinterpret_cast<const std::byte*>(source.data()) + offset);
        }
    }

    void ensureScene(const std::shared_ptr<const StaticScene>& next) {
        if (scene == next) {
            return;
        }
        renderer.beginCommandBatch();
        upload(buffers.xyz_ws, next->positions);
        upload(buffers.rotations, next->rotations);
        upload(buffers.scaling_raw, next->logScales);
        upload(buffers.opacity_raw, next->opacityLogits);
        upload(buffers.sh0, next->sh0);
        // shN, overlays and the unused projection slots all share one zero fill.
        const std::vector<float> zeros(1024, 0);
        upload(dummy, zeros);
        upload(overlays, zeros);
        upload(buffers.shN, zeros);
        renderer.endCommandBatch(true);
        scene = next;
        buffers.num_splats = scene->count();
        ++uploads;
    }

    void prepareReadback(std::size_t bytes) {
        if (readbackCapacity >= bytes) {
            return;
        }
        if (readback) {
            vmaDestroyBuffer(allocator, readback, readbackAllocation);
        }
        readback = VK_NULL_HANDLE;
        readbackAllocation = VK_NULL_HANDLE;
        mapped = nullptr;
        readbackCapacity = 0;

        auto bufferInfo = vkInfo<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
        bufferInfo.size = bytes;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        auto allocation = hostVisibleAllocation();
        VmaAllocationInfo allocated{};
        vkCheck(
            vmaCreateBuffer(allocator, &bufferInfo, &allocation, &readback, &readbackAllocation, &allocated),
            "create pixel readback");
        mapped = allocated.pMappedData;
        readbackCapacity = bytes;
    }

    void refreshTelemetryWindow(FrameReport& report) {
        const auto now = Clock::now();
        sampleCounters = lastCounterSample.time_since_epoch().count() == 0 ||
                         now - lastCounterSample >= telemetrySampleInterval;
        if (sampleCounters) {
            lastCounterSample = now;
        }
        report.backend.telemetryAgeMs =
            lastCounterSample.time_since_epoch().count() == 0
                ? 0.
                : std::chrono::duration<double, std::milli>(now - lastCounterSample).count();
    }

    void resetBackendTimers() {
        // Zeroed before ensureScene() can fire command-batch timers, so upload waits are counted.
        backendWaitMs = 0;
        queryReadbackCpuMs = 0;
    }

    void resetStageTimers() {
        gpuMs = 0;
        projectionMs = 0;
        sortingMs = 0;
        rasterMs = 0;
    }

    CameraFrame buildCameraFrame(const FrameState& f) const {
        CameraFrame frame{};
        auto& u = frame.uniforms;
        u.image_width = f.width;
        u.camera_width = f.width;
        u.image_height = f.height;
        u.camera_height = f.height;
        u.grid_width = (f.width + TILE_WIDTH - 1) / TILE_WIDTH;
        u.grid_height = (f.height + TILE_HEIGHT - 1) / TILE_HEIGHT;
        u.num_splats = static_cast<std::uint32_t>(scene->count());
        u.model_num_splats = u.num_splats;
        u.sort_capacity = HIGS_DEPTH_WAVE_INSTANCES;
        u.color_exposure = 1;
        u.rasterization_scale = 1;
        const auto view = cameraView(f.camera, f.width, f.height);
        u.fx = view.focalPixels;
        u.fy = u.fx;
        u.cx = static_cast<float>(f.width) * .5f;
        u.cy = static_cast<float>(f.height) * .5f;

        frame.orbitDegrees = std::fmod(f.camera.yawDegrees + fullTurnDegrees, fullTurnDegrees);
        // Upstream shader expects column-major world->camera (camera Z forward, Y down).
        for (std::size_t index = 0; index < view.worldToCamera.size(); ++index) {
            u.world_view_transform[index] = view.worldToCamera[index];
        }
        return frame;
    }

    // Projection, depth sort and the exact tile-instance gate. Returns the gate's raw instance count.
    std::size_t recordProjection(const VulkanGSRendererUniforms& u) {
        renderer.beginCommandBatch();
        currentTiles = u.grid_width * u.grid_height;
        if (sampleCounters) {
            renderer.clearDeviceBuffer(tileTotals, currentTiles + tileTotalsClearMargin);
        }

        // Optional upstream slots the static SH0 path does not use: per-splat transforms,
        // node masks and LOD selection all point at the shared zero-filled `dummy` buffer.
        auto& unused = dummy.deviceBuffer;
        renderer.executeProjectionForward(u,
                                          buffers,
                                          /*transform_indices=*/unused,
                                          /*node_mask=*/unused,
                                          /*overlay_params=*/overlays.deviceBuffer,
                                          /*model_transforms=*/unused,
                                          /*alloc_reserve=*/0,
                                          /*use_gut_projection=*/false,
                                          /*lod_indices=*/{},
                                          /*lod_logical_indices=*/{},
                                          /*lod_levels=*/{},
                                          /*lod_weights=*/{},
                                          /*lod_counts=*/{},
                                          /*write_overlay_flags=*/false);
        renderer.executeSortPrimitivesByDepth(u, buffers);
        renderer.executeApplyDepthOrdering(u, buffers);
        renderer.executeCalculateIndexBufferOffset(u, buffers);

        const auto gate =
            renderer.synchronizeTileInstanceGate(buffers); // exact count; submits and waits batch A
        if (gate.count_overflow) {
            throw std::runtime_error("Tile-instance count overflow");
        }
        buffers.num_indices = gate.raw_count;
        return gate.raw_count;
    }

    // Legacy full-frame rasterizer: one wave per depth rank, radix-sorted by tile reference width.
    std::size_t recordWaveRasterization(const VulkanGSRendererUniforms& u, std::size_t references) {
        const std::size_t tiles = currentTiles;
        const std::size_t waves =
            lfs::rendering::vulkan::indirect_layout::depthWaveRecordUpperBound(references, tiles);
        const auto sortBits =
            std::max(static_cast<int>(sortBitsMinimum), static_cast<int>(std::bit_width(tiles)));
        auto& unused = dummy.deviceBuffer;
        renderer.executeWavePartition(u, buffers, waves, false);
        renderer.executeLegacyDepthWaves(u,
                                         buffers,
                                         waves,
                                         sortBits,
                                         /*selection_mask=*/unused,
                                         /*preview_mask=*/unused,
                                         /*selection_colors=*/unused,
                                         /*overlay_flags=*/buffers.overlay_flags.deviceBuffer,
                                         /*overlay_params=*/overlays.deviceBuffer,
                                         /*transform_indices=*/unused,
                                         /*model_transforms=*/unused,
                                         /*use_gut_rasterization=*/false,
                                         /*overlays_active=*/false,
                                         /*predicate_waves=*/false);
        return waves;
    }

    RasterScope recordGaussianPass(const VulkanGSRendererUniforms& u) {
        const std::size_t references = recordProjection(u);
        const std::size_t waves = recordWaveRasterization(u, references);
        return {references, waves, currentTiles};
    }

    void recordCompose(VkCommandBuffer command,
                       const FrameState& f,
                       VkImage image,
                       VkImageView imageView,
                       bool initialized) {
        const lfs::rendering::vulkan::DeclaredAccess pixelAccess{
            &buffers.pixel_state.deviceBuffer, lfs::rendering::vulkan::BufferUse::ComputeRead};
        renderer.planTransfer(std::span{&pixelAccess, 1});
        transitionTarget(
            command, image, initialized ? qtReadableTarget() : undefinedTarget(), computeTarget());

        const VkDescriptorBufferInfo pixelState{buffers.pixel_state.deviceBuffer.buffer,
                                                buffers.pixel_state.deviceBuffer.offset,
                                                buffers.pixel_state.deviceBuffer.size};
        const VkDescriptorImageInfo target{VK_NULL_HANDLE, imageView, VK_IMAGE_LAYOUT_GENERAL};
        const VkWriteDescriptorSet writes[] = {storageBufferWrite(0, pixelState),
                                               storageImageWrite(1, target)};

        if (composeQueries) {
            vkCmdResetQueryPool(command, composeQueries, 0, timestampQueryCount);
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, composeQueries, 0);
        }
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, compose);
        vkCmdPushDescriptorSetKHR(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 2, writes);
        const std::uint32_t extent[] = {f.width, f.height};
        vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, composePushBytes, extent);
        vkCmdDispatch(command,
                      (f.width + composeWorkgroupSize - 1) / composeWorkgroupSize,
                      (f.height + composeWorkgroupSize - 1) / composeWorkgroupSize,
                      1);
        if (composeQueries) {
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, composeQueries, 1);
        }
    }

    void recordTileTotalsReadback(VkCommandBuffer command) {
        using namespace lfs::rendering::vulkan;
        const DeclaredAccess readAccess[] = {{&tileTotals.deviceBuffer, BufferUse::TransferRead},
                                             {&buffers.visible_count.deviceBuffer, BufferUse::TransferRead}};
        renderer.planTransfer(readAccess);
        // `counters` layout: [0]=max tile list, [4]=total references, [8]=visible count.
        VkBufferCopy totals{tileTotals.deviceBuffer.offset, 0, tileTotalsReadbackBytes};
        vkCmdCopyBuffer(command, tileTotals.deviceBuffer.buffer, counters, 1, &totals);
        VkBufferCopy visible{
            buffers.visible_count.deviceBuffer.offset, visibleCountFieldOffset, sizeof(std::uint32_t)};
        vkCmdCopyBuffer(command, buffers.visible_count.deviceBuffer.buffer, counters, 1, &visible);
        releaseToHost(command, counters, countersBufferSizeBytes);
    }

    void recordPixelReadback(VkCommandBuffer command, const FrameState& f, VkImage image) {
        prepareReadback(static_cast<std::size_t>(f.width) * f.height * targetBytesPerPixel);
        transitionTarget(command, image, computeTarget(), copySourceTarget());
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {f.width, f.height, 1};
        vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
        releaseToHost(command, readback, VK_WHOLE_SIZE);
    }

    void readCapture(const FrameState& f) {
        vkCheck(vmaInvalidateAllocation(allocator, readbackAllocation, 0, VK_WHOLE_SIZE),
                "invalidate capture");
        auto result = std::make_shared<FrameCapture>();
        result->requestId = f.captureRequestId;
        result->width = f.width;
        result->height = f.height;
        const auto* pixels = static_cast<const std::uint8_t*>(mapped);
        result->rgba.assign(pixels,
                            pixels + static_cast<std::size_t>(f.width) * f.height * targetBytesPerPixel);
        capture = std::move(result);
    }

    void readTileTotals(FrameReport& report, const RasterScope& scope) {
        vkCheck(vmaInvalidateAllocation(allocator, countersAllocation, 0, VK_WHOLE_SIZE),
                "invalidate statistics");
        const auto* counts = static_cast<const std::uint32_t*>(counterMap);
        auto& backend = report.backend;
        backend.visibleGaussians = counts[2];
        backend.telemetryReferences = counts[1];
        backend.maxTileList = counts[0];
        backend.telemetryFrameIndex = report.frame.frameIndex + 1;
        if (counts[1] != scope.references) {
            backend.telemetryError = "Emitted references=" + std::to_string(counts[1]) +
                                     "; projection gate=" + std::to_string(scope.references) +
                                     "; tiles=" + std::to_string(scope.tiles);
        }
    }

    void readComposeTimestamp(FrameReport& report) {
        report.frame.composeGpuMs.reset();
        if (!composeQueries) {
            return;
        }
        std::uint64_t ticks[timestampQueryCount]{};
        vkCheck(vkGetQueryPoolResults(context.device,
                                      composeQueries,
                                      0,
                                      timestampQueryCount,
                                      sizeof(ticks),
                                      ticks,
                                      sizeof(std::uint64_t),
                                      VK_QUERY_RESULT_64_BIT),
                "compose timestamps");
        const auto mask = timestampBits >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << timestampBits) - 1;
        report.frame.composeGpuMs = static_cast<double>((ticks[1] - ticks[0]) & mask) * timestampPeriod / 1e6;
    }

    void refreshBufferCensus(FrameReport& report) {
        if (lastCensusAllocations == renderer.getBufferAllocationCount()) {
            return;
        }
        auto& backend = report.backend;
        const auto owned = buffers.getOwnedVramBreakdown();
        const auto sum = [&owned](std::initializer_list<const char*> names) {
            std::size_t total = 0;
            for (const auto* name : names) {
                const auto entry = owned.find(name);
                if (entry != owned.end()) {
                    total += entry->second;
                }
            }
            return total;
        };
        backend.modelBytes = sum({"xyz_ws", "sh0", "shN", "rotations", "scaling_raw", "opacity_raw"});
        backend.preprocessingBytes =
            sum({"rect_tile_space", "xy_vs", "depths", "inv_cov_vs_opacity", "rgb", "overlay_flags"});
        backend.pixelBytes = sum({"pixel_state",
                                  "pixel_depth",
                                  "pixel_depth_weight",
                                  "n_contributors",
                                  "tile_batch_pixel_state",
                                  "tile_batch_n_contributors"});
        const auto capacity = buffers.getTotalOwnedAllocSize();
        backend.sortingBytes =
            capacity - backend.modelBytes - backend.preprocessingBytes - backend.pixelBytes;
        if (lastCapacity && capacity > lastCapacity) {
            ++growths;
        }
        lastCapacity = capacity;
        backend.gsGrowths = growths;
        lastCensusAllocations = renderer.getBufferAllocationCount();
    }

    void fillReport(FrameReport& report, const CameraFrame& camera, const RasterScope& scope) {
        auto& frame = report.frame;
        auto& backend = report.backend;
        backend.telemetryBytes =
            tileTotals.deviceBuffer.allocSize + countersBufferSizeBytes + readbackCapacity;
        frame.gaussianCount = scene->count();
        backend.tileInstances = scope.references;
        backend.depthWaves = scope.waves;
        frame.sceneUploads = uploads;
        backend.gsReservedBytes =
            buffers.getTotalOwnedAllocSize() + dummy.deviceBuffer.allocSize + overlays.deviceBuffer.allocSize;
        frame.orbitDegrees = camera.orbitDegrees;
        frame.capture = capture;
        frame.gpuRenderMs = measured(gpuMs);
        frame.projectionGpuMs = measured(projectionMs);
        frame.sortingGpuMs = measured(sortingMs);
        frame.rasterGpuMs = measured(rasterMs);
        frame.backendWaitMs = backendWaitMs;
        frame.queryReadbackCpuMs = queryReadbackCpuMs;
        backend.gsBufferAllocations = renderer.getBufferAllocationCount();
        backend.gsBufferReuses = renderer.getBufferReuseCount();
    }

    void render(const FrameState& f,
                VkImage image,
                VkImageView imageView,
                bool initialized,
                FrameReport& report) {
        refreshTelemetryWindow(report);
        resetBackendTimers();
        ensureScene(f.scene);
        resetStageTimers();

        const auto camera = buildCameraFrame(f);
        const bool captureRequested =
            f.captureRequestId && (!capture || capture->requestId != f.captureRequestId);
        const auto scope = recordGaussianPass(camera.uniforms);

        // Recorded after the gate's batch-A submit/wait, which starts a fresh command buffer.
        const auto command = renderer.activeCommandBuffer();
        recordCompose(command, f, image, imageView, initialized);
        if (sampleCounters) {
            recordTileTotalsReadback(command);
        }
        if (captureRequested) {
            recordPixelReadback(command, f, image);
        }
        transitionTarget(
            command, image, captureRequested ? copySourceTarget() : computeTarget(), qtReadableTarget());
        renderer.endCommandBatch(true); // all depth waves and compose finished before Qt sampling

        if (captureRequested) {
            readCapture(f);
        }
        if (sampleCounters) {
            readTileTotals(report, scope);
        }
        readComposeTimestamp(report);
        refreshBufferCensus(report);
        fillReport(report, camera, scope);
    }
};

StaticGsRenderer::StaticGsRenderer(DeviceContext c, VmaAllocator a) : impl_(std::make_unique<Impl>(c, a)) {
    impl_->initialize();
}

StaticGsRenderer::~StaticGsRenderer() = default;

void StaticGsRenderer::render(const FrameState& f,
                              VkImage image,
                              VkImageView view,
                              bool initialized,
                              FrameReport& report) {
    impl_->render(f, image, view, initialized, report);
}
} // namespace orbelio
