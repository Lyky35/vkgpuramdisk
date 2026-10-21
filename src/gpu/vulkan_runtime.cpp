#include "gpu/vulkan_runtime.h"

#include <algorithm>
#include <format>

#include "core/log.h"
#include "gpu/vulkan_drive.h"

namespace grd {

std::wstring GpuDeviceInfo::typeName() const {
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            return L"Discrete GPU";
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            return L"Integrated GPU";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            return L"Virtual GPU";
        case VK_PHYSICAL_DEVICE_TYPE_CPU:
            return L"CPU";
        default:
            return L"Other";
    }
}

VulkanRuntime& VulkanRuntime::instance() {
    static VulkanRuntime runtime;
    return runtime;
}

VulkanRuntime::~VulkanRuntime() {
    shutdown();
}

bool VulkanRuntime::initialize(std::wstring& error) {
    std::lock_guard lock(mutex_);
    if (initialized_) {
        error = error_;
        return error_.empty();
    }

    initialized_ = true;
    error_.clear();

    loader_ = UniqueLibrary(
        LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
    if (!loader_) {
        error_ = L"Vulkan loader vulkan-1.dll was not found. Install a GPU "
                 L"driver with Vulkan support.";
        error = error_;
        return false;
    }

    std::string missing;
    if (!loadGlobalVulkanApi(api_, loader_.get(), missing)) {
        error_ = L"Vulkan loader is missing " + widen(missing);
        error = error_;
        return false;
    }

    uint32_t loaderVersion = VK_API_VERSION_1_0;
    if (api_.EnumerateInstanceVersion != nullptr) {
        if (api_.EnumerateInstanceVersion(&loaderVersion) != VK_SUCCESS) {
            loaderVersion = VK_API_VERSION_1_0;
        }
    }
    Log::debug(std::format(L"Vulkan loader version {}.{}.{}",
                           VK_API_VERSION_MAJOR(loaderVersion),
                           VK_API_VERSION_MINOR(loaderVersion),
                           VK_API_VERSION_PATCH(loaderVersion)));

    const VkApplicationInfo applicationInfo = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "GpuRamDriveVk",
        .applicationVersion = 1,
        .pEngineName = "GpuRamDriveVk",
        .engineVersion = 1,
        .apiVersion = VK_API_VERSION_1_0,
    };

    const VkInstanceCreateInfo createInfo = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &applicationInfo,
        .enabledLayerCount = 0,
        .ppEnabledLayerNames = nullptr,
        .enabledExtensionCount = 0,
        .ppEnabledExtensionNames = nullptr,
    };

    const VkResult createResult =
        api_.CreateInstance(&createInfo, nullptr, &instance_);
    if (createResult != VK_SUCCESS) {
        error_ = L"Unable to create a Vulkan instance: Vulkan error " +
                 std::to_wstring(createResult);
        error = error_;
        return false;
    }

    if (!resolveInstanceVulkanApi(api_, instance_, missing)) {
        error_ = L"Vulkan instance is missing " + widen(missing);
        error = error_;
        return false;
    }

    uint32_t count = 0;
    VkResult result = api_.EnumeratePhysicalDevices(instance_, &count, nullptr);
    if (result != VK_SUCCESS || count == 0) {
        error_ = L"No Vulkan physical devices were found.";
        error = error_;
        return false;
    }

    physicalDevices_.resize(count);
    result = api_.EnumeratePhysicalDevices(instance_, &count,
                                           physicalDevices_.data());
    if (result != VK_SUCCESS) {
        error_ = L"Unable to enumerate Vulkan physical devices: Vulkan error " +
                 std::to_wstring(result);
        error = error_;
        return false;
    }
    physicalDevices_.resize(count);

    devices_.reserve(physicalDevices_.size());
    for (uint32_t index = 0; index < physicalDevices_.size(); ++index) {
        VkPhysicalDeviceProperties properties = {};
        api_.GetPhysicalDeviceProperties(physicalDevices_[index], &properties);

        VkPhysicalDeviceMemoryProperties memory = {};
        api_.GetPhysicalDeviceMemoryProperties(physicalDevices_[index], &memory);

        GpuDeviceInfo info;
        info.index = index;
        info.name = widen(properties.deviceName);
        info.type = properties.deviceType;
        info.apiVersion = properties.apiVersion;
        info.driverVersion = properties.driverVersion;
        info.vendorId = properties.vendorID;
        info.deviceId = properties.deviceID;
        info.hostVisibleDeviceLocal = false;

        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
            const VkMemoryPropertyFlags flags = memory.memoryTypes[i].propertyFlags;
            if ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 &&
                (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 &&
                (flags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) == 0) {
                info.hostVisibleDeviceLocal = true;
            }
        }

        for (uint32_t heap = 0; heap < memory.memoryHeapCount; ++heap) {
            bool deviceLocal = false;
            for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
                if (memory.memoryTypes[i].heapIndex == heap &&
                    (memory.memoryTypes[i].propertyFlags &
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
                    deviceLocal = true;
                    break;
                }
            }
            if (deviceLocal) {
                info.deviceLocalBytes =
                    std::max(info.deviceLocalBytes, memory.memoryHeaps[heap].size);
            }
        }

        Log::debug(std::format(
            L"GPU {}: {} ({}, {} MiB device-local, direct map: {})", index,
            info.name, info.typeName(), info.deviceLocalBytes / (1024 * 1024),
            info.hostVisibleDeviceLocal ? L"yes" : L"no"));

        devices_.push_back(std::move(info));
    }

    return true;
}

std::unique_ptr<VulkanDrive> VulkanRuntime::createDrive(uint32_t deviceIndex,
                                                        uint64_t sizeBytes,
                                                        std::wstring& error) {
    if (!initialize(error)) {
        return nullptr;
    }
    if (deviceIndex >= physicalDevices_.size()) {
        error = std::format(L"GPU {} does not exist; {} device(s) present.",
                            deviceIndex, physicalDevices_.size());
        return nullptr;
    }

    return VulkanDrive::create(api_, physicalDevices_[deviceIndex],
                               devices_[deviceIndex], sizeBytes, error);
}

void VulkanRuntime::shutdown() {
    if (instance_ != VK_NULL_HANDLE && api_.DestroyInstance != nullptr) {
        api_.DestroyInstance(instance_, nullptr);
    }
    instance_ = VK_NULL_HANDLE;
    physicalDevices_.clear();
    devices_.clear();
    loader_ = UniqueLibrary();
}

}  // namespace grd
