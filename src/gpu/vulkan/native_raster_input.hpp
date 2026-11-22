#pragma once
#include "native_gpu_resources.hpp"

namespace orbelio {
struct NativeRasterInput {
    const NativeBuffer& projected;
    const NativeBuffer& keys;
    const NativeBuffer& indices;
    std::uint32_t count;
    const NativeBuffer* visibility = nullptr;
    const NativeBuffer* normals = nullptr;
};

struct NativeTileQueries {
    static constexpr std::uint32_t count = 4;
    VkQueryPool pool = VK_NULL_HANDLE;
    std::uint32_t first = 0;
};
}
