#pragma once
#include "vma.hpp"
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>

namespace orbelio {
enum class NativeHostAccess { None, Upload, Readback };

struct NativeBuffer {
    VkBuffer handle = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    std::size_t capacity = 0;
    void* mapped = nullptr;
};

struct NativeBufferCounts {
    std::uint64_t allocations = 0;
    std::uint64_t reuses = 0;
    std::uint64_t growths = 0;
};

struct NativePipeline {
    VkDescriptorSetLayout descriptors = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline handle = VK_NULL_HANDLE;
};

void destroyBuffer(VmaAllocator allocator, NativeBuffer& buffer) noexcept;
void reserveBuffer(VmaAllocator allocator,
                   NativeBuffer& buffer,
                   std::size_t bytes,
                   VkBufferUsageFlags usage,
                   NativeHostAccess host,
                   NativeBufferCounts& counts);
VkDeviceAddress bufferAddress(DeviceContext context, const NativeBuffer& buffer);
void destroyPipeline(DeviceContext context, NativePipeline& pipeline) noexcept;
VkShaderModule loadNativeShader(DeviceContext context, std::string_view directory, std::string_view name);
void createNativeLayout(DeviceContext context,
                        NativePipeline& pipeline,
                        std::span<const VkDescriptorType> types,
                        VkShaderStageFlags stages,
                        std::uint32_t pushBytes);
void createNativeCompute(DeviceContext context,
                         NativePipeline& pipeline,
                         std::string_view directory,
                         std::string_view name,
                         std::uint32_t bindings,
                         std::uint32_t pushBytes);
void bindNativeBuffers(VkCommandBuffer command,
                       const NativePipeline& pipeline,
                       VkPipelineBindPoint bindPoint,
                       std::initializer_list<const NativeBuffer*> buffers);
void nativeMemoryBarrier(VkCommandBuffer command,
                         VkPipelineStageFlags sourceStage,
                         VkAccessFlags sourceAccess,
                         VkPipelineStageFlags destinationStage,
                         VkAccessFlags destinationAccess);
}
