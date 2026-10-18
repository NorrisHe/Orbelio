#pragma once

#include <volk.h>
#include <stdexcept>
#include <string>
#include <thread>

namespace orbelio {
template <class T> T vkInfo(VkStructureType type) {
    T value{};
    value.sType = type;
    return value;
}

inline void vkCheck(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(result));
    }
}

// Borrowed from the GUI/platform bootstrap. Must outlive all engine resources.
struct DeviceContext {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queueFamily = 0;
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
