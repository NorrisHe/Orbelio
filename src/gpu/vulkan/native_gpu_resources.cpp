#include "native_gpu_resources.hpp"
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace orbelio {
namespace {
constexpr std::size_t minimumBufferBytes = sizeof(std::uint32_t);
constexpr std::size_t maximumBufferBindings = 7;

std::vector<std::uint32_t> shaderCode(std::string_view directory, std::string_view name) {
    const std::string_view rootText = directory.empty() ? ORBELIO_NATIVE_SHADER_DIR : directory;
    const std::filesystem::path root(std::u8string(rootText.begin(), rootText.end()));
    const auto path = root / std::u8string(name.begin(), name.end());
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const auto bytes = static_cast<std::streamoff>(file.tellg());
    if (!file || bytes <= 0 || bytes % sizeof(std::uint32_t)) {
        throw std::runtime_error("Cannot read native shader: " + std::string(rootText) + "/" +
                                 std::string(name));
    }
    std::vector<std::uint32_t> code(static_cast<std::size_t>(bytes) / sizeof(std::uint32_t));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(code.data()), bytes)) {
        throw std::runtime_error("Incomplete native shader: " + std::string(rootText) + "/" +
                                 std::string(name));
    }
    return code;
}
}

void destroyBuffer(VmaAllocator allocator, NativeBuffer& buffer) noexcept {
    if (buffer.handle) {
        vmaDestroyBuffer(allocator, buffer.handle, buffer.allocation);
    }
    buffer = {};
}

void reserveBuffer(VmaAllocator allocator,
                   NativeBuffer& buffer,
                   std::size_t bytes,
                   VkBufferUsageFlags usage,
                   NativeHostAccess host,
                   NativeBufferCounts& counts) {
    bytes = std::max(bytes, minimumBufferBytes);
    if (buffer.capacity >= bytes) {
        ++counts.reuses;
        return;
    }
    const bool growing = buffer.handle != VK_NULL_HANDLE;
    destroyBuffer(allocator, buffer);
    auto info = vkInfo<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    info.size = bytes;
    info.usage = usage;
    VmaAllocationCreateInfo memory{};
    memory.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
    memory.usage = host == NativeHostAccess::None ? VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE
                                                  : VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    if (host != NativeHostAccess::None) {
        memory.flags |=
            VMA_ALLOCATION_CREATE_MAPPED_BIT |
            (host == NativeHostAccess::Upload ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                                              : VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT);
    }
    VmaAllocationInfo allocation{};
    const auto result =
        vmaCreateBuffer(allocator, &info, &memory, &buffer.handle, &buffer.allocation, &allocation);
    if (result != VK_SUCCESS) {
        throw VulkanError(result, ("vmaCreateBuffer native (" + std::to_string(bytes) + " bytes)").c_str());
    }
    buffer.capacity = bytes;
    buffer.mapped = allocation.pMappedData;
    ++counts.allocations;
    counts.growths += growing;
}

VkDeviceAddress bufferAddress(DeviceContext context, const NativeBuffer& buffer) {
    auto info = vkInfo<VkBufferDeviceAddressInfo>(VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO);
    info.buffer = buffer.handle;
    return vkGetBufferDeviceAddress(context.device, &info);
}

void destroyPipeline(DeviceContext context, NativePipeline& pipeline) noexcept {
    vkDestroyPipeline(context.device, pipeline.handle, nullptr);
    vkDestroyPipelineLayout(context.device, pipeline.layout, nullptr);
    vkDestroyDescriptorSetLayout(context.device, pipeline.descriptors, nullptr);
    pipeline = {};
}

VkShaderModule loadNativeShader(DeviceContext context, std::string_view directory, std::string_view name) {
    const auto code = shaderCode(directory, name);
    auto info = vkInfo<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    info.codeSize = code.size() * sizeof(std::uint32_t);
    info.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    vkCheck(vkCreateShaderModule(context.device, &info, nullptr, &module), "vkCreateShaderModule native");
    return module;
}

void createNativeLayout(DeviceContext context,
                        NativePipeline& pipeline,
                        std::span<const VkDescriptorType> types,
                        VkShaderStageFlags stages,
                        std::uint32_t pushBytes) {
    std::vector<VkDescriptorSetLayoutBinding> bindings(types.size());
    for (std::size_t i = 0; i < types.size(); ++i) {
        bindings[i].binding = static_cast<std::uint32_t>(i);
        bindings[i].descriptorCount = 1;
        bindings[i].descriptorType = types[i];
        bindings[i].stageFlags = stages;
    }
    auto descriptors =
        vkInfo<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    descriptors.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    descriptors.bindingCount = static_cast<std::uint32_t>(bindings.size());
    descriptors.pBindings = bindings.data();
    vkCheck(vkCreateDescriptorSetLayout(context.device, &descriptors, nullptr, &pipeline.descriptors),
            "vkCreateDescriptorSetLayout native");
    const VkPushConstantRange range{stages, 0, pushBytes};
    auto layout = vkInfo<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &pipeline.descriptors;
    layout.pushConstantRangeCount = pushBytes ? 1 : 0;
    layout.pPushConstantRanges = pushBytes ? &range : nullptr;
    vkCheck(vkCreatePipelineLayout(context.device, &layout, nullptr, &pipeline.layout),
            "vkCreatePipelineLayout native");
}

void createNativeCompute(DeviceContext context,
                         NativePipeline& pipeline,
                         std::string_view directory,
                         std::string_view name,
                         std::uint32_t bindings,
                         std::uint32_t pushBytes) {
    std::array<VkDescriptorType, maximumBufferBindings> types{};
    types.fill(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    createNativeCompute(context, pipeline, directory, name, std::span(types).first(bindings), pushBytes);
}

void createNativeCompute(DeviceContext context,
                         NativePipeline& pipeline,
                         std::string_view directory,
                         std::string_view name,
                         std::span<const VkDescriptorType> types,
                         std::uint32_t pushBytes) {
    createNativeLayout(context, pipeline, types, VK_SHADER_STAGE_COMPUTE_BIT, pushBytes);
    const auto shader = loadNativeShader(context, directory, name);
    auto info = vkInfo<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
    info.layout = pipeline.layout;
    info.stage = vkInfo<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = shader;
    info.stage.pName = "main";
    const auto result =
        vkCreateComputePipelines(context.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline.handle);
    vkDestroyShaderModule(context.device, shader, nullptr);
    vkCheck(result, "vkCreateComputePipelines native");
}

void bindNativeBuffers(VkCommandBuffer command,
                       const NativePipeline& pipeline,
                       VkPipelineBindPoint bindPoint,
                       std::initializer_list<const NativeBuffer*> buffers,
                       const NativeBuffer* control) {
    std::array<VkDescriptorBufferInfo, maximumBufferBindings> infos{};
    std::array<VkWriteDescriptorSet, maximumBufferBindings> writes{};
    std::size_t i = 0;
    const auto append = [&infos, &writes, &i](const NativeBuffer* buffer) {
        infos[i] = {buffer->handle, 0, buffer->capacity};
        writes[i] = vkInfo<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        writes[i].dstBinding = static_cast<std::uint32_t>(i);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &infos[i];
        ++i;
    };
    for (const auto* buffer : buffers) {
        append(buffer);
    }
    if (control) {
        append(control);
    }
    vkCmdBindPipeline(command, bindPoint, pipeline.handle);
    vkCmdPushDescriptorSetKHR(
        command, bindPoint, pipeline.layout, 0, static_cast<std::uint32_t>(i), writes.data());
}

void nativeImageBarrier(VkCommandBuffer command,
                        VkPipelineStageFlags sourceStage,
                        VkPipelineStageFlags destinationStage,
                        const VkImageMemoryBarrier& barrier) {
    vkCmdPipelineBarrier(command, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void nativeMemoryBarrier(VkCommandBuffer command,
                         VkPipelineStageFlags sourceStage,
                         VkAccessFlags sourceAccess,
                         VkPipelineStageFlags destinationStage,
                         VkAccessFlags destinationAccess) {
    auto barrier = vkInfo<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
    barrier.srcAccessMask = sourceAccess;
    barrier.dstAccessMask = destinationAccess;
    vkCmdPipelineBarrier(command, sourceStage, destinationStage, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}
}
