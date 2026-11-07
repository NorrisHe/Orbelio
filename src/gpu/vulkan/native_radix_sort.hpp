#pragma once
#include "native_gpu_resources.hpp"
#include <array>
#include <string_view>

namespace orbelio {
struct NativeSortControl {
    std::uint32_t count = 0;
    std::uint32_t flags = 0;
    std::array<std::uint32_t, 3> histogramDispatch{0, 1, 1};
    std::array<std::uint32_t, 3> prefixDispatch{256, 0, 1};
};

static_assert(sizeof(NativeSortControl) == 32);

struct NativeVisibilityControl {
    NativeSortControl sort;
    VkDrawIndirectCommand draw{};
};

static_assert(sizeof(NativeVisibilityControl) == 48);
enum class NativeSortMode { FixedCount, GpuCount, VisibleDepths };

// Records a stable uint32 key/value sort; borrows the renderer's Device and allocator.
class NativeRadixSort {
public:
    NativeRadixSort(DeviceContext context,
                    VmaAllocator allocator,
                    NativeBufferCounts& counts,
                    std::string_view shaderDirectory = {},
                    NativeSortMode mode = NativeSortMode::FixedCount);
    ~NativeRadixSort();
    NativeRadixSort(const NativeRadixSort&) = delete;
    NativeRadixSort& operator=(const NativeRadixSort&) = delete;

    void reserve(std::uint32_t maximumCount);
    // keyBits may omit only high bits known to be zero in every active key.
    // VisibleDepths rejects sentinel 0xffffffff keys and writes NativeVisibilityControl; ties stay stable.
    void record(VkCommandBuffer command,
                std::uint32_t count,
                const NativeBuffer& keys,
                const NativeBuffer& values,
                const NativeBuffer* control = nullptr,
                std::uint32_t keyBits = 32);
    std::size_t bytes() const;

private:
    void createPipelines(std::string_view directory);
    void createVisiblePass(std::string_view directory);
    void destroy() noexcept;
    DeviceContext context_;
    VmaAllocator allocator_;
    NativePipeline histogram_;
    NativePipeline prefix_;
    NativePipeline offsets_;
    NativePipeline scatter_;
    std::array<NativePipeline, 4> visiblePass_{};
    std::array<NativeBuffer, 6> buffers_{};
    NativeBufferCounts& counts_;
    NativeSortMode mode_;
};
}
