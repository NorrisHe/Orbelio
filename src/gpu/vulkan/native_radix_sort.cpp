#include "native_radix_sort.hpp"
#include <numeric>
#include <algorithm>

namespace orbelio {
namespace {
constexpr std::uint32_t groupSize = 256;
constexpr std::uint32_t radix = 256;
constexpr std::uint32_t passBits = 8;
constexpr std::uint32_t histogramTileGroups = 32;

struct Parameters {
    std::uint32_t count;
    std::uint32_t groups;
    std::uint32_t chunks;
    std::uint32_t shift;
    std::uint32_t activeRadix;
};

struct DispatchGrid {
    std::uint32_t x = 1;
    std::uint32_t y = 1;
    VkDeviceSize indirectOffset = 0;
};

void computeBarrier(VkCommandBuffer command, bool publishesIndirect) {
    nativeMemoryBarrier(command,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                            (publishesIndirect ? VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT : 0),
                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                            (publishesIndirect ? VK_ACCESS_INDIRECT_COMMAND_READ_BIT : 0));
}

void dispatch(VkCommandBuffer command,
              const NativePipeline& pipeline,
              const Parameters& parameters,
              const NativeBuffer* control,
              DispatchGrid grid,
              bool publishesIndirect = false) {
    vkCmdPushConstants(
        command, pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(parameters), &parameters);
    if (control) {
        vkCmdDispatchIndirect(command, control->handle, grid.indirectOffset);
    } else {
        vkCmdDispatch(command, grid.x, grid.y, 1);
    }
    computeBarrier(command, publishesIndirect);
}
}

NativeRadixSort::NativeRadixSort(DeviceContext context,
                                 VmaAllocator allocator,
                                 NativeBufferCounts& counts,
                                 std::string_view directory,
                                 NativeSortMode mode)
    : context_(context), allocator_(allocator), counts_(counts), mode_(mode) {
    try {
        createPipelines(directory);
    } catch (...) {
        destroy();
        throw;
    }
}

void NativeRadixSort::createPipelines(std::string_view directory) {
    const bool gpuCount = mode_ != NativeSortMode::FixedCount;
    createNativeCompute(context_,
                        histogram_,
                        directory,
                        gpuCount ? "native_histogram_dynamic.comp.spv" : "native_histogram.comp.spv",
                        2 + gpuCount,
                        sizeof(Parameters));
    createNativeCompute(context_,
                        prefix_,
                        directory,
                        gpuCount ? "native_prefix_dynamic.comp.spv" : "native_prefix.comp.spv",
                        3 + gpuCount,
                        sizeof(Parameters));
    createNativeCompute(context_,
                        offsets_,
                        directory,
                        gpuCount ? "native_offsets_dynamic.comp.spv" : "native_offsets.comp.spv",
                        2 + gpuCount,
                        sizeof(Parameters));
    createNativeCompute(context_,
                        scatter_,
                        directory,
                        gpuCount ? "native_scatter_dynamic.comp.spv" : "native_scatter.comp.spv",
                        6 + gpuCount,
                        sizeof(Parameters));
    if (mode_ == NativeSortMode::VisibleDepths) {
        createVisiblePass(directory);
    }
}

NativeRadixSort::~NativeRadixSort() { destroy(); }

void NativeRadixSort::createVisiblePass(std::string_view directory) {
    const std::array names{"native_histogram_visible.comp.spv",
                           "native_prefix.comp.spv",
                           "native_offsets_visible.comp.spv",
                           "native_scatter_visible.comp.spv"};
    constexpr std::array bindings{2u, 3u, 3u, 6u};
    for (std::size_t i = 0; i < visiblePass_.size(); ++i) {
        createNativeCompute(context_, visiblePass_[i], directory, names[i], bindings[i], sizeof(Parameters));
    }
}

void NativeRadixSort::destroy() noexcept {
    for (auto* pipeline : {&histogram_, &prefix_, &offsets_, &scatter_}) {
        destroyPipeline(context_, *pipeline);
    }
    for (auto& pipeline : visiblePass_) {
        destroyPipeline(context_, pipeline);
    }
    for (auto& buffer : buffers_) {
        destroyBuffer(allocator_, buffer);
    }
}

void NativeRadixSort::reserve(std::uint32_t maximumCount) {
    const auto groups = (maximumCount + groupSize - 1) / groupSize;
    const auto paddedGroups = (groups + histogramTileGroups - 1) / histogramTileGroups * histogramTileGroups;
    const auto chunks = (groups + groupSize - 1) / groupSize;
    const std::array<std::size_t, 6> sizes{maximumCount,
                                           maximumCount,
                                           std::size_t(paddedGroups) * radix,
                                           std::size_t(paddedGroups) * radix,
                                           std::size_t(chunks) * radix,
                                           std::size_t(chunks) * radix};
    for (std::size_t i = 0; i < buffers_.size(); ++i) {
        reserveBuffer(allocator_,
                      buffers_[i],
                      sizes[i] * sizeof(std::uint32_t),
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      NativeHostAccess::None,
                      counts_);
    }
}

std::size_t NativeRadixSort::bytes() const {
    return std::accumulate(buffers_.begin(), buffers_.end(), std::size_t{}, [](auto sum, const auto& buffer) {
        return sum + buffer.capacity;
    });
}

void NativeRadixSort::record(VkCommandBuffer command,
                             std::uint32_t count,
                             const NativeBuffer& keys,
                             const NativeBuffer& values,
                             const NativeBuffer* control,
                             std::uint32_t keyBits) {
    if (!keyBits || keyBits > 32) {
        throw std::invalid_argument("Unsupported radix key width");
    }
    if ((mode_ != NativeSortMode::FixedCount) != bool(control)) {
        throw std::invalid_argument("Sort count source does not match its pipeline");
    }
    if (mode_ == NativeSortMode::FixedCount && count < 2) {
        return;
    }
    const auto groups = (count + groupSize - 1) / groupSize;
    Parameters parameters{count, groups, (groups + groupSize - 1) / groupSize, 0, radix};
    const NativeBuffer* inputKeys = &keys;
    const NativeBuffer* inputValues = &values;
    const NativeBuffer* outputKeys = &buffers_[0];
    const NativeBuffer* outputValues = &buffers_[1];
    const auto& histogram = buffers_[2];
    const auto& prefix = buffers_[3];
    const auto& totals = buffers_[4];
    const auto& offsets = buffers_[5];
    // An even number of passes leaves the result in the caller's buffers, including narrow keys.
    const auto roundedBits = (keyBits + 15) / 16 * 16;
    for (parameters.shift = 0; parameters.shift < roundedBits; parameters.shift += passBits) {
        const auto remaining = keyBits > parameters.shift ? keyBits - parameters.shift : 0u;
        parameters.activeRadix = 1u << std::min(passBits, remaining);
        // Filtering the first stable radix pass compacts without another full-size scatter buffer.
        const bool filter = mode_ == NativeSortMode::VisibleDepths && parameters.shift == 0;
        const auto* liveControl = filter ? nullptr : control;
        const auto& histogramPipeline = filter ? visiblePass_[0] : histogram_;
        const auto& prefixPipeline = filter ? visiblePass_[1] : prefix_;
        const auto& offsetsPipeline = filter ? visiblePass_[2] : offsets_;
        const auto& scatterPipeline = filter ? visiblePass_[3] : scatter_;
        bindNativeBuffers(
            command, histogramPipeline, VK_PIPELINE_BIND_POINT_COMPUTE, {inputKeys, &histogram}, liveControl);
        dispatch(command,
                 histogramPipeline,
                 parameters,
                 liveControl,
                 {groups, 1, offsetof(NativeSortControl, histogramDispatch)});
        bindNativeBuffers(command,
                          prefixPipeline,
                          VK_PIPELINE_BIND_POINT_COMPUTE,
                          {&histogram, &prefix, &totals},
                          liveControl);
        dispatch(command,
                 prefixPipeline,
                 parameters,
                 liveControl,
                 {parameters.activeRadix, parameters.chunks, offsetof(NativeSortControl, prefixDispatch)});
        bindNativeBuffers(
            command, offsetsPipeline, VK_PIPELINE_BIND_POINT_COMPUTE, {&totals, &offsets}, control);
        dispatch(command, offsetsPipeline, parameters, nullptr, {}, filter);
        bindNativeBuffers(command,
                          scatterPipeline,
                          VK_PIPELINE_BIND_POINT_COMPUTE,
                          {inputKeys, inputValues, &prefix, &offsets, outputKeys, outputValues},
                          liveControl);
        dispatch(command,
                 scatterPipeline,
                 parameters,
                 liveControl,
                 {groups, 1, offsetof(NativeSortControl, histogramDispatch)});
        std::swap(inputKeys, outputKeys);
        std::swap(inputValues, outputValues);
    }
}
}
