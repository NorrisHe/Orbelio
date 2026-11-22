#include "native_frame_commands.hpp"
#include <chrono>
#include <limits>
#include <utility>
#include <vector>

namespace orbelio {
namespace {
using Clock = std::chrono::steady_clock;
constexpr auto fenceWaitNs = std::numeric_limits<std::uint64_t>::max();

double milliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

}

NativeFrameCommands::NativeFrameCommands(DeviceContext context) : context_(context) {
    try {
        createCommands();
        createQueries();
        createStatisticsQueries();
    } catch (...) {
        destroy();
        throw;
    }
}

NativeFrameCommands::~NativeFrameCommands() { destroy(); }

void NativeFrameCommands::destroy() noexcept {
    vkDestroyQueryPool(context_.device, queries_, nullptr);
    vkDestroyQueryPool(context_.device, statisticsQueries_, nullptr);
    vkDestroyFence(context_.device, fence_, nullptr);
    vkDestroyCommandPool(context_.device, commands_, nullptr);
}

NativeTileQueries NativeFrameCommands::tileQueries(bool capture) const {
    return {capture ? queries_ : VK_NULL_HANDLE, frameTimestampCount};
}

void NativeFrameCommands::createCommands() {
    auto pool = vkInfo<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
    pool.queueFamilyIndex = context_.queueFamily;
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCheck(vkCreateCommandPool(context_.device, &pool, nullptr, &commands_), "vkCreateCommandPool native");
    auto allocation = vkInfo<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
    allocation.commandPool = commands_;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    vkCheck(vkAllocateCommandBuffers(context_.device, &allocation, &command_),
            "vkAllocateCommandBuffers native");
    auto info = vkInfo<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
    vkCheck(vkCreateFence(context_.device, &info, nullptr, &fence_), "vkCreateFence native");
}

void NativeFrameCommands::createQueries() {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(context_.physicalDevice, &properties);
    timestampPeriod_ = properties.limits.timestampPeriod;
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(context_.physicalDevice, &count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(count);
    vkGetPhysicalDeviceQueueFamilyProperties(context_.physicalDevice, &count, queues.data());
    const auto bits = queues[context_.queueFamily].timestampValidBits;
    if (!bits || !timestampPeriod_) {
        return;
    }
    timestampMask_ = bits == 64 ? std::numeric_limits<std::uint64_t>::max() : (std::uint64_t{1} << bits) - 1;
    auto info = vkInfo<VkQueryPoolCreateInfo>(VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = frameTimestampCount + NativeTileQueries::count;
    vkCheck(vkCreateQueryPool(context_.device, &info, nullptr, &queries_), "vkCreateQueryPool native");
}

void NativeFrameCommands::createStatisticsQueries() {
    if (!context_.pipelineStatisticsEnabled) {
        return;
    }
    auto info = vkInfo<VkQueryPoolCreateInfo>(VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);
    info.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
    info.queryCount = 1;
    info.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
    vkCheck(vkCreateQueryPool(context_.device, &info, nullptr, &statisticsQueries_),
            "vkCreateQueryPool fragment statistics");
}

VkCommandBuffer NativeFrameCommands::begin() {
    vkCheck(vkResetCommandPool(context_.device, commands_, 0), "vkResetCommandPool native");
    auto info = vkInfo<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkCheck(vkBeginCommandBuffer(command_, &info), "vkBeginCommandBuffer native");
    if (queries_) {
        vkCmdResetQueryPool(command_, queries_, 0, frameTimestampCount + NativeTileQueries::count);
    }
    return command_;
}

void NativeFrameCommands::stamp(Stamp stage) {
    if (queries_) {
        vkCmdWriteTimestamp(
            command_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, queries_, static_cast<std::uint32_t>(stage));
    }
}

double NativeFrameCommands::submitAndWait() {
    vkCheck(vkEndCommandBuffer(command_), "vkEndCommandBuffer native");
    vkCheck(vkResetFences(context_.device, 1, &fence_), "vkResetFences native");
    auto info = vkInfo<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
    info.commandBufferCount = 1;
    info.pCommandBuffers = &command_;
    vkCheck(vkQueueSubmit(context_.queue, 1, &info, fence_), "vkQueueSubmit native");
    const auto start = Clock::now();
    vkCheck(vkWaitForFences(context_.device, 1, &fence_, VK_TRUE, fenceWaitNs), "vkWaitForFences native");
    return milliseconds(start);
}

void NativeFrameCommands::readTicks(std::uint32_t first, std::span<std::uint64_t> values) {
    vkCheck(vkGetQueryPoolResults(context_.device,
                                  queries_,
                                  first,
                                  static_cast<std::uint32_t>(values.size()),
                                  values.size_bytes(),
                                  values.data(),
                                  sizeof(values[0]),
                                  VK_QUERY_RESULT_64_BIT),
            "vkGetQueryPoolResults native");
}

void NativeFrameCommands::readTileTimings(BackendStats& stats,
                                          const std::array<std::uint64_t, frameTimestampCount>& frame) {
    std::array<std::uint64_t, NativeTileQueries::count> phases{};
    readTicks(frameTimestampCount, phases);
    const std::array ticks{frame[std::to_underlying(Stamp::SortEnd)],
                           phases[0],
                           phases[1],
                           phases[2],
                           phases[3],
                           frame[std::to_underlying(Stamp::BinningEnd)]};
    const auto interval = [&](std::size_t i) {
        return double((ticks[i + 1] - ticks[i]) & timestampMask_) * timestampPeriod_ / 1'000'000.;
    };
    stats.tileStageTimings =
        TileStageTimings{interval(0), interval(1), interval(2), interval(3), interval(4)};
}

void NativeFrameCommands::readTimestamps(const FrameState& state,
                                         FrameReport& report,
                                         bool tiled,
                                         bool captured) {
    auto& frame = report.frame;
    report.backend.tileStageTimings.reset();
    frame.projectionGpuMs.reset();
    frame.shadingGpuMs.reset();
    frame.sortingGpuMs.reset();
    frame.binningGpuMs.reset();
    frame.rasterGpuMs.reset();
    frame.composeGpuMs.reset();
    frame.gpuRenderMs.reset();
    if (!queries_) {
        return;
    }
    std::array<std::uint64_t, frameTimestampCount> values{};
    readTicks(0, values);
    if (tiled && captured) {
        readTileTimings(report.backend, values);
    }
    const auto interval = [&](Stamp begin, Stamp end) {
        const auto ticks = values[std::to_underlying(end)] - values[std::to_underlying(begin)];
        return double(ticks & timestampMask_) * timestampPeriod_ / 1'000'000.;
    };
    const auto shadingMs = interval(Stamp::ProjectionEnd, Stamp::ShadingEnd);
    const auto binningMs = interval(Stamp::SortEnd, Stamp::BinningEnd);
    frame.projectionGpuMs = interval(Stamp::ProjectionBegin, Stamp::ProjectionEnd);
    if (state.renderSettings.separateShading && state.renderSettings.attributeView == AttributeView::Normal) {
        frame.shadingGpuMs = shadingMs;
    }
    frame.sortingGpuMs = interval(Stamp::ShadingEnd, Stamp::SortEnd);
    if (tiled) {
        frame.binningGpuMs = binningMs;
    }
    frame.rasterGpuMs = interval(Stamp::BinningEnd, Stamp::RasterEnd);
    frame.composeGpuMs = interval(Stamp::RasterEnd, Stamp::CompositionEnd);
    frame.gpuRenderMs =
        *frame.projectionGpuMs + shadingMs + *frame.sortingGpuMs + binningMs + *frame.rasterGpuMs;
}

void NativeFrameCommands::readStatistics(BackendStats& stats, bool captured) {
    stats.fragmentShaderInvocations.reset();
    if (!captured || !statisticsQueries_) {
        return;
    }
    std::uint64_t invocations = 0;
    vkCheck(vkGetQueryPoolResults(context_.device,
                                  statisticsQueries_,
                                  0,
                                  1,
                                  sizeof(invocations),
                                  &invocations,
                                  sizeof(invocations),
                                  VK_QUERY_RESULT_64_BIT),
            "read fragment statistics");
    stats.fragmentShaderInvocations = invocations;
}

}
