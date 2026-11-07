#pragma once

#include <volk.h>
#include <stdexcept>
#include <string>
#include <thread>

namespace orbelio {
class VulkanError final : public std::runtime_error {
public:
    VulkanError(VkResult result, const char* operation)
        : std::runtime_error(std::string(operation) + " failed: " + resultText(result)), result_(result) {}

    VkResult result() const { return result_; }

private:
    static std::string resultText(VkResult result) {
        const auto code = std::to_string(result);
        switch (result) {
        case VK_ERROR_OUT_OF_HOST_MEMORY:
            return "VK_ERROR_OUT_OF_HOST_MEMORY (" + code + ")";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:
            return "VK_ERROR_OUT_OF_DEVICE_MEMORY (" + code + ")";
        case VK_ERROR_DEVICE_LOST:
            return "VK_ERROR_DEVICE_LOST (" + code + ")";
        default:
            return code;
        }
    }

    VkResult result_;
};

template <class T> T vkInfo(VkStructureType type) {
    T value{};
    value.sType = type;
    return value;
}

inline void vkCheck(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw VulkanError(result, operation);
    }
}

// Borrowed from the GUI/platform bootstrap. Must outlive all engine resources.
// Use one graphics+compute queue, enable bufferDeviceAddress and VK_KHR_push_descriptor,
// and load the device entry points through volk before creating a renderer.
struct DeviceContext {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queueFamily = 0;
    bool pipelineStatisticsEnabled = false;
};

class RenderThreadGuard {
public:
    void check() const {
        if (std::this_thread::get_id() != owner_) {
            throw std::logic_error("GPU resource used outside its render thread");
        }
    }

private:
    std::thread::id owner_ = std::this_thread::get_id();
};
} // namespace orbelio
