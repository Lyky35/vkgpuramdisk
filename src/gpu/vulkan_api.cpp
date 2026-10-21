#include "gpu/vulkan_api.h"

namespace grd {
namespace {

template <typename T>
void resolve(T& target, PFN_vkGetInstanceProcAddr getProc, VkInstance instance,
             const char* name, std::string& missing) {
    if (target != nullptr) {
        return;
    }
    target = reinterpret_cast<T>(getProc(instance, name));
    if (target == nullptr && missing.empty()) {
        missing = name;
    }
}

}  // namespace

bool loadGlobalVulkanApi(VulkanApi& api, HMODULE loader, std::string& missing) {
    missing.clear();
    api.GetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(loader, "vkGetInstanceProcAddr"));
    if (api.GetInstanceProcAddr == nullptr) {
        missing = "vkGetInstanceProcAddr";
        return false;
    }

    api.CreateInstance = reinterpret_cast<PFN_vkCreateInstance>(
        api.GetInstanceProcAddr(nullptr, "vkCreateInstance"));
    if (api.CreateInstance == nullptr) {
        missing = "vkCreateInstance";
        return false;
    }

    // Vulkan 1.0 has no vkEnumerateInstanceVersion. Its absence is not an error.
    api.EnumerateInstanceVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        api.GetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
    return true;
}

bool resolveInstanceVulkanApi(VulkanApi& api, VkInstance instance,
                              std::string& missing) {
    missing.clear();
    const PFN_vkGetInstanceProcAddr getProc = api.GetInstanceProcAddr;
    if (getProc == nullptr) {
        missing = "vkGetInstanceProcAddr";
        return false;
    }

    resolve(api.DestroyInstance, getProc, instance, "vkDestroyInstance", missing);
    resolve(api.EnumeratePhysicalDevices, getProc, instance,
            "vkEnumeratePhysicalDevices", missing);
    resolve(api.GetPhysicalDeviceProperties, getProc, instance,
            "vkGetPhysicalDeviceProperties", missing);
    resolve(api.GetPhysicalDeviceMemoryProperties, getProc, instance,
            "vkGetPhysicalDeviceMemoryProperties", missing);
    resolve(api.GetPhysicalDeviceQueueFamilyProperties, getProc, instance,
            "vkGetPhysicalDeviceQueueFamilyProperties", missing);
    resolve(api.CreateDevice, getProc, instance, "vkCreateDevice", missing);

    resolve(api.DestroyDevice, getProc, instance, "vkDestroyDevice", missing);
    resolve(api.GetDeviceQueue, getProc, instance, "vkGetDeviceQueue", missing);
    resolve(api.DeviceWaitIdle, getProc, instance, "vkDeviceWaitIdle", missing);
    resolve(api.CreateCommandPool, getProc, instance, "vkCreateCommandPool",
            missing);
    resolve(api.DestroyCommandPool, getProc, instance, "vkDestroyCommandPool",
            missing);
    resolve(api.AllocateCommandBuffers, getProc, instance,
            "vkAllocateCommandBuffers", missing);
    resolve(api.FreeCommandBuffers, getProc, instance, "vkFreeCommandBuffers",
            missing);
    resolve(api.BeginCommandBuffer, getProc, instance, "vkBeginCommandBuffer",
            missing);
    resolve(api.EndCommandBuffer, getProc, instance, "vkEndCommandBuffer", missing);
    resolve(api.CmdCopyBuffer, getProc, instance, "vkCmdCopyBuffer", missing);
    resolve(api.CmdFillBuffer, getProc, instance, "vkCmdFillBuffer", missing);
    resolve(api.CmdPipelineBarrier, getProc, instance, "vkCmdPipelineBarrier",
            missing);
    resolve(api.CreateBuffer, getProc, instance, "vkCreateBuffer", missing);
    resolve(api.DestroyBuffer, getProc, instance, "vkDestroyBuffer", missing);
    resolve(api.GetBufferMemoryRequirements, getProc, instance,
            "vkGetBufferMemoryRequirements", missing);
    resolve(api.AllocateMemory, getProc, instance, "vkAllocateMemory", missing);
    resolve(api.FreeMemory, getProc, instance, "vkFreeMemory", missing);
    resolve(api.BindBufferMemory, getProc, instance, "vkBindBufferMemory", missing);
    resolve(api.MapMemory, getProc, instance, "vkMapMemory", missing);
    resolve(api.UnmapMemory, getProc, instance, "vkUnmapMemory", missing);
    resolve(api.FlushMappedMemoryRanges, getProc, instance,
            "vkFlushMappedMemoryRanges", missing);
    resolve(api.InvalidateMappedMemoryRanges, getProc, instance,
            "vkInvalidateMappedMemoryRanges", missing);
    resolve(api.CreateFence, getProc, instance, "vkCreateFence", missing);
    resolve(api.DestroyFence, getProc, instance, "vkDestroyFence", missing);
    resolve(api.WaitForFences, getProc, instance, "vkWaitForFences", missing);
    resolve(api.ResetFences, getProc, instance, "vkResetFences", missing);
    resolve(api.QueueSubmit, getProc, instance, "vkQueueSubmit", missing);

    return missing.empty();
}

}  // namespace grd
