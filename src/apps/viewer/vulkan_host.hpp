#pragma once

#include <device_context.hpp>
#include <QVulkanInstance>
#include <QString>
#include <atomic>
#include <cstdint>
#include <optional>
#include <vector>

class QQuickWindow;

namespace orbelio {
class VulkanHost {
public:
    explicit VulkanHost(bool validation);
    ~VulkanHost();
    void attach(QQuickWindow& window);

    DeviceContext context() const { return context_; }

    QString deviceName() const { return deviceName_; }

    bool validationEnabled() const { return validation_; }

    unsigned validationErrors() const { return validationErrors_.load(); }

private:
    // Everything the GS backend requires from a candidate device, flattened so the
    // query results can leave the pNext chain that produced them.
    struct Admission {
        VkPhysicalDeviceProperties properties{};
        bool pushDescriptors = false;
        bool synchronization2 = false;
        bool timelineSemaphore = false;
        bool bufferDeviceAddress = false;
        bool pipelineStatistics = false;
        bool memoryBudget = false;
    };

    // Instance bootstrap, in the order Vulkan 1.3 + Qt require: choose layers and extensions,
    // arm the error counter, then create and load the dispatch table.
    void configureLayersAndDebug(bool validationRequested);
    void installValidationErrorCounter();
    void createInstance();
    void prepareNativeSurface(QQuickWindow& window);
    std::vector<VkPhysicalDevice> enumeratePhysicalDevices() const;
    // nullopt means the device cannot run the GS pipeline.
    std::optional<Admission> probeAdmission(VkPhysicalDevice physical) const;
    // Non-const: Qt's supportsPresent() may resolve the platform loader and surface lazily.
    std::vector<std::uint32_t> presentationFamilies(VkPhysicalDevice physical, QQuickWindow& window);
    void createLogicalDevice(VkPhysicalDevice physical,
                             std::uint32_t queueFamily,
                             const Admission& admission,
                             QQuickWindow& window);

    QVulkanInstance instance_;
    DeviceContext context_;
    QString deviceName_;
    bool validation_ = false;
    std::atomic_uint validationErrors_{0};
};
} // namespace orbelio
