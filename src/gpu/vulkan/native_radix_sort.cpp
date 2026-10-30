#include "native_radix_sort.hpp"
#include <numeric>

namespace orbelio {
namespace {
constexpr std::uint32_t groupSize = 256;
constexpr std::uint32_t radix = 256;
constexpr std::uint32_t passBits = 8;
constexpr std::uint32_t keyBits = 32;

struct Parameters {
    std::uint32_t count;
    std::uint32_t groups;
    std::uint32_t chunks;
    std::uint32_t shift;
};

void computeBarrier(VkCommandBuffer command) {
    nativeMemoryBarrier(command,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}

void dispatch(VkCommandBuffer command,
              const NativePipeline& pipeline,
              const Parameters& parameters,
              std::uint32_t x,
              std::uint32_t y = 1) {
    vkCmdPushConstants(
        command, pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(parameters), &parameters);
    vkCmdDispatch(command, x, y, 1);
    computeBarrier(command);
}
}

NativeRadixSort::NativeRadixSort(DeviceContext context,
                                 VmaAllocator allocator,
                                 NativeBufferCounts& counts,
                                 std::string_view directory)
    : context_(context), allocator_(allocator), counts_(counts) {
    try {
        createNativeCompute(
            context_, histogram_, directory, "native_histogram.comp.spv", 2, sizeof(Parameters));
        createNativeCompute(context_, prefix_, directory, "native_prefix.comp.spv", 3, sizeof(Parameters));
        createNativeCompute(context_, offsets_, directory, "native_offsets.comp.spv", 2, sizeof(Parameters));
        createNativeCompute(context_, scatter_, directory, "native_scatter.comp.spv", 6, sizeof(Parameters));
    } catch (...) {
        destroy();
        throw;
    }
}

NativeRadixSort::~NativeRadixSort() { destroy(); }

void NativeRadixSort::destroy() noexcept {
    for (auto* pipeline : {&histogram_, &prefix_, &offsets_, &scatter_}) {
        destroyPipeline(context_, *pipeline);
    }
    for (auto& buffer : buffers_) {
        destroyBuffer(allocator_, buffer);
    }
}

void NativeRadixSort::reserve(std::uint32_t maximumCount) {
    const auto groups = (maximumCount + groupSize - 1) / groupSize;
    const auto chunks = (groups + groupSize - 1) / groupSize;
    const std::array<std::size_t, 6> sizes{maximumCount,
                                           maximumCount,
                                           std::size_t(groups) * radix,
                                           std::size_t(groups) * radix,
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
                             const NativeBuffer& values) {
    if (count < 2) {
        return;
    }
    const auto groups = (count + groupSize - 1) / groupSize;
    Parameters parameters{count, groups, (groups + groupSize - 1) / groupSize, 0};
    const NativeBuffer* inputKeys = &keys;
    const NativeBuffer* inputValues = &values;
    const NativeBuffer* outputKeys = &buffers_[0];
    const NativeBuffer* outputValues = &buffers_[1];
    const auto& histogram = buffers_[2];
    const auto& prefix = buffers_[3];
    const auto& totals = buffers_[4];
    const auto& offsets = buffers_[5];
    for (parameters.shift = 0; parameters.shift < keyBits; parameters.shift += passBits) {
        bindNativeBuffers(command, histogram_, VK_PIPELINE_BIND_POINT_COMPUTE, {inputKeys, &histogram});
        dispatch(command, histogram_, parameters, groups);
        bindNativeBuffers(command, prefix_, VK_PIPELINE_BIND_POINT_COMPUTE, {&histogram, &prefix, &totals});
        dispatch(command, prefix_, parameters, parameters.chunks, radix);
        bindNativeBuffers(command, offsets_, VK_PIPELINE_BIND_POINT_COMPUTE, {&totals, &offsets});
        dispatch(command, offsets_, parameters, 1);
        bindNativeBuffers(command,
                          scatter_,
                          VK_PIPELINE_BIND_POINT_COMPUTE,
                          {inputKeys, inputValues, &prefix, &offsets, outputKeys, outputValues});
        dispatch(command, scatter_, parameters, groups);
        std::swap(inputKeys, outputKeys);
        std::swap(inputValues, outputValues);
    }
}
}
