#pragma once
#include "native_gpu_resources.hpp"
#include <array>
#include <string_view>

namespace orbelio {
// Records a stable uint32 key/value sort; borrows the renderer's Device and allocator.
class NativeRadixSort {
public:
    NativeRadixSort(DeviceContext context,
                    VmaAllocator allocator,
                    NativeBufferCounts& counts,
                    std::string_view shaderDirectory = {});
    ~NativeRadixSort();
    NativeRadixSort(const NativeRadixSort&) = delete;
    NativeRadixSort& operator=(const NativeRadixSort&) = delete;

    void reserve(std::uint32_t maximumCount);
    void record(VkCommandBuffer command,
                std::uint32_t count,
                const NativeBuffer& keys,
                const NativeBuffer& values);
    std::size_t bytes() const;

private:
    void destroy() noexcept;
    DeviceContext context_;
    VmaAllocator allocator_;
    NativePipeline histogram_;
    NativePipeline prefix_;
    NativePipeline offsets_;
    NativePipeline scatter_;
    std::array<NativeBuffer, 6> buffers_{};
    NativeBufferCounts& counts_;
};
}
