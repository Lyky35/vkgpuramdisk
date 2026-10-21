#pragma once

#include <windows.h>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/util.h"
#include "gpu/vulkan_api.h"

namespace grd {

class VulkanDrive;

struct GpuDeviceInfo {
    uint32_t index = 0;
    std::wstring name;
    VkPhysicalDeviceType type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    uint32_t apiVersion = 0;
    uint32_t driverVersion = 0;
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;
    uint64_t deviceLocalBytes = 0;
    bool hostVisibleDeviceLocal = false;

    std::wstring typeName() const;
};

class VulkanRuntime {
public:
    static VulkanRuntime& instance();

    VulkanRuntime(const VulkanRuntime&) = delete;
    VulkanRuntime& operator=(const VulkanRuntime&) = delete;

    bool initialize(std::wstring& error);
    bool initialized() const { return initialized_; }
    const std::wstring& lastError() const { return error_; }

    const std::vector<GpuDeviceInfo>& devices() const { return devices_; }

    std::unique_ptr<VulkanDrive> createDrive(uint32_t deviceIndex,
                                             uint64_t sizeBytes,
                                             std::wstring& error);

    ~VulkanRuntime();

private:
    VulkanRuntime() = default;

    void shutdown();

    std::mutex mutex_;
    bool initialized_ = false;
    std::wstring error_;
    UniqueLibrary loader_;
    VulkanApi api_;
    VkInstance instance_ = VK_NULL_HANDLE;
    std::vector<VkPhysicalDevice> physicalDevices_;
    std::vector<GpuDeviceInfo> devices_;
};

}  // namespace grd
