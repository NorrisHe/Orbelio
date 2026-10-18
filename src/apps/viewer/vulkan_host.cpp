#include "vulkan_host.hpp"
#include <QQuickGraphicsDevice>
#include <QQuickWindow>
#include <algorithm>
#include <string_view>

namespace orbelio {
namespace {
constexpr std::uint32_t requiredPushConstantBytes = 224;
constexpr VkQueueFlags requiredQueueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;

bool hasExtension(const std::vector<VkExtensionProperties>& available, std::string_view name) {
    return std::any_of(available.begin(), available.end(), [&](const auto& extension) {
        return std::string_view(extension.extensionName) == name;
    });
}
} // namespace

VulkanHost::VulkanHost(bool validation) {
    configureLayersAndDebug(validation);
    installValidationErrorCounter();
    createInstance();
}

void VulkanHost::configureLayersAndDebug(bool validationRequested) {
    instance_.setApiVersion(QVersionNumber(1, 3, 0));
    validation_ = validationRequested && instance_.supportedLayers().contains("VK_LAYER_KHRONOS_validation");
    if (validationRequested && !validation_) {
        throw std::runtime_error(
            "Validation requested but VK_LAYER_KHRONOS_validation is unavailable; run scripts/run.ps1");
    }
    if (validation_) {
        instance_.setLayers({"VK_LAYER_KHRONOS_validation"});
    }
    if (instance_.supportedExtensions().contains(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        instance_.setExtensions({VK_EXT_DEBUG_UTILS_EXTENSION_NAME});
    }
}

void VulkanHost::installValidationErrorCounter() {
    instance_.installDebugOutputFilter(
        QVulkanInstance::DebugUtilsFilter([this](QVulkanInstance::DebugMessageSeverityFlags severity,
                                                 QVulkanInstance::DebugMessageTypeFlags,
                                                 const void*) {
            if (severity.testFlag(QVulkanInstance::ErrorSeverity)) {
                ++validationErrors_;
            }
            return false; // Preserve Qt's diagnostic output.
        }));
}

void VulkanHost::createInstance() {
    if (!instance_.create()) {
        throw std::runtime_error("Could not create Vulkan 1.3 instance");
    }
    vkCheck(volkInitialize(), "volkInitialize");
    volkLoadInstance(instance_.vkInstance());
    context_.instance = instance_.vkInstance();
}

void VulkanHost::prepareNativeSurface(QQuickWindow& window) {
    window.setVulkanInstance(&instance_);
    // Presentation support needs an actual native window/surface, before exposure.
    window.setSurfaceType(QSurface::VulkanSurface);
    window.create();
}

std::vector<VkPhysicalDevice> VulkanHost::enumeratePhysicalDevices() const {
    std::uint32_t count = 0;
    vkCheck(vkEnumeratePhysicalDevices(context_.instance, &count, nullptr), "vkEnumeratePhysicalDevices");
    std::vector<VkPhysicalDevice> devices(count);
    vkCheck(vkEnumeratePhysicalDevices(context_.instance, &count, devices.data()),
            "vkEnumeratePhysicalDevices");
    return devices;
}

std::optional<VulkanHost::Admission> VulkanHost::probeAdmission(VkPhysicalDevice physical) const {
    Admission admission;
    vkGetPhysicalDeviceProperties(physical, &admission.properties);
    if (admission.properties.apiVersion < VK_API_VERSION_1_3) {
        return std::nullopt;
    }

    auto supported13 =
        vkInfo<VkPhysicalDeviceVulkan13Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
    auto supported12 =
        vkInfo<VkPhysicalDeviceVulkan12Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    supported12.pNext = &supported13;
    auto supported11 =
        vkInfo<VkPhysicalDeviceVulkan11Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
    supported11.pNext = &supported12;
    auto supported = vkInfo<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
    supported.pNext = &supported11;
    vkGetPhysicalDeviceFeatures2(physical, &supported);

    std::uint32_t extensionCount = 0;
    vkCheck(vkEnumerateDeviceExtensionProperties(physical, nullptr, &extensionCount, nullptr),
            "enumerate device extensions");
    std::vector<VkExtensionProperties> available(extensionCount);
    vkCheck(vkEnumerateDeviceExtensionProperties(physical, nullptr, &extensionCount, available.data()),
            "read device extensions");

    admission.pushDescriptors = hasExtension(available, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    admission.synchronization2 = supported13.synchronization2;
    admission.timelineSemaphore = supported12.timelineSemaphore;
    admission.shaderInt64 = supported.features.shaderInt64;
    admission.shaderInt16 = supported.features.shaderInt16;
    admission.storageBuffer16BitAccess = supported11.storageBuffer16BitAccess;
    admission.uniformAndStorageBuffer16BitAccess = supported11.uniformAndStorageBuffer16BitAccess;
    admission.shaderFloat16 = supported12.shaderFloat16;
    admission.subgroupSizeControl = supported13.subgroupSizeControl;
    admission.computeFullSubgroups = supported13.computeFullSubgroups;

    if (!admission.pushDescriptors || !admission.synchronization2 || !admission.timelineSemaphore ||
        !admission.shaderInt64 || !admission.shaderInt16 || !admission.storageBuffer16BitAccess ||
        admission.properties.limits.maxPushConstantsSize < requiredPushConstantBytes) {
        return std::nullopt;
    }
    return admission;
}

std::vector<std::uint32_t> VulkanHost::presentationFamilies(VkPhysicalDevice physical, QQuickWindow& window) {
    std::uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, families.data());
    std::vector<std::uint32_t> usable;
    for (std::uint32_t family = 0; family < familyCount; ++family) {
        if ((families[family].queueFlags & requiredQueueFlags) != requiredQueueFlags) {
            continue;
        }
        if (!instance_.supportsPresent(physical, family, &window)) {
            continue;
        }
        usable.push_back(family);
    }
    return usable;
}

void VulkanHost::createLogicalDevice(VkPhysicalDevice physical,
                                     std::uint32_t queueFamily,
                                     const Admission& admission,
                                     QQuickWindow& window) {
    const float priority = 1.0f;
    auto queueInfo = vkInfo<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
    queueInfo.queueFamilyIndex = queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    auto f13 =
        vkInfo<VkPhysicalDeviceVulkan13Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
    f13.synchronization2 = VK_TRUE;
    f13.subgroupSizeControl = admission.subgroupSizeControl;
    f13.computeFullSubgroups = admission.computeFullSubgroups;
    auto f12 =
        vkInfo<VkPhysicalDeviceVulkan12Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    f12.pNext = &f13;
    f12.timelineSemaphore = VK_TRUE;
    f12.shaderFloat16 = admission.shaderFloat16;
    auto f11 =
        vkInfo<VkPhysicalDeviceVulkan11Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
    f11.pNext = &f12;
    f11.storageBuffer16BitAccess = VK_TRUE;
    f11.uniformAndStorageBuffer16BitAccess = admission.uniformAndStorageBuffer16BitAccess;

    VkPhysicalDeviceFeatures features{};
    features.shaderInt16 = VK_TRUE;
    features.shaderInt64 = VK_TRUE;

    const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    auto info = vkInfo<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queueInfo;
    info.pNext = &f11;
    info.pEnabledFeatures = &features;
    info.enabledExtensionCount = 2;
    info.ppEnabledExtensionNames = extensions;

    vkCheck(vkCreateDevice(physical, &info, nullptr, &context_.device), "vkCreateDevice");
    context_.physicalDevice = physical;
    context_.queueFamily = queueFamily;
    volkLoadDevice(context_.device);
    vkGetDeviceQueue(context_.device, queueFamily, 0, &context_.queue);
    deviceName_ = QString::fromUtf8(admission.properties.deviceName);
    window.setGraphicsDevice(
        QQuickGraphicsDevice::fromDeviceObjects(physical, context_.device, static_cast<int>(queueFamily), 0));
}

void VulkanHost::attach(QQuickWindow& window) {
    if (context_.device) {
        throw std::logic_error("Vulkan host already attached");
    }
    prepareNativeSurface(window);
    for (auto physical : enumeratePhysicalDevices()) {
        const auto admission = probeAdmission(physical);
        if (!admission) {
            continue;
        }
        for (const auto family : presentationFamilies(physical, window)) {
            createLogicalDevice(physical, family, *admission, window);
            return;
        }
    }
    throw std::runtime_error("No suitable Vulkan 1.3 GPU: GS requires push descriptors, 224-byte push "
                             "constants, int16/int64, 16-bit storage, timeline and sync2");
}

VulkanHost::~VulkanHost() {
    // The QQuickWindow must already be destroyed, joining its render thread.
    if (context_.device) {
        vkDeviceWaitIdle(context_.device);
        instance_.resetDeviceFunctions(context_.device);
        vkDestroyDevice(context_.device, nullptr);
    }
}
} // namespace orbelio
