#pragma once

#include <windows.h>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "gpu/vulkan_api.h"
#include "gpu/vulkan_runtime.h"

namespace grd {

// A block of memory that lives on a Vulkan device and is exposed as a flat,
// byte-addressable range. On a discrete GPU the memory is usually not host
// visible, so reads and writes are staged through a small host-visible buffer.
// On an integrated GPU (or a discrete GPU with resizable BAR) the device-local
// memory is mapped directly.
class VulkanDrive {
public:
    static std::unique_ptr<VulkanDrive> create(VulkanApi& api,
                                               VkPhysicalDevice physicalDevice,
                                               const GpuDeviceInfo& info,
                                               uint64_t sizeBytes,
                                               std::wstring& error);

    ~VulkanDrive();

    VulkanDrive(const VulkanDrive&) = delete;
    VulkanDrive& operator=(const VulkanDrive&) = delete;

    uint64_t size() const { return size_; }
    bool directMapped() const { return direct_; }
    const std::wstring& info() const { return infoText_; }

    void read(uint64_t offset, void* destination, size_t length);
    void write(uint64_t offset, const void* source, size_t length);

private:
    struct Segment {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize allocationSize = 0;
        VkDeviceSize logicalSize = 0;
        uint64_t offset = 0;
        void* mapped = nullptr;
        bool coherent = false;
    };

    VulkanDrive(VulkanApi& api, VkPhysicalDevice physicalDevice,
                const GpuDeviceInfo& info);

    void initialize(uint64_t sizeBytes);
    void cleanup();

    void createLogicalDevice();
    void createCommandObjects();
    void allocateStorage(uint64_t sizeBytes);
    void allocateStagingBuffer();
    void zeroStorage();

    uint32_t chooseMemoryType(uint32_t allowedBits, bool requireDeviceLocal);
    uint32_t chooseStagingMemoryType(uint32_t allowedBits);

    Segment& locate(uint64_t offset, uint64_t& localOffset,
                    uint64_t& available);

    void fillSegment(const Segment& segment);
    void submitCopy(VkBuffer source, VkDeviceSize sourceOffset, VkBuffer dest,
                    VkDeviceSize destOffset, VkDeviceSize size,
                    bool transferToDevice);
    void waitForFence();
    void flushRange(const Segment& segment, uint64_t offset, uint64_t length);
    void invalidateRange(const Segment& segment, uint64_t offset,
                         uint64_t length);
    void flushStaging(uint64_t offset, uint64_t length);
    void invalidateStaging(uint64_t offset, uint64_t length);
    void mappedMemoryBarrier(VkDeviceMemory memory, uint64_t offset,
                             uint64_t length, bool flush);

    VulkanApi& api_;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    GpuDeviceInfo info_;

    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = UINT32_MAX;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;

    VkPhysicalDeviceProperties properties_ = {};
    VkPhysicalDeviceMemoryProperties memoryProperties_ = {};
    VkDeviceSize nonCoherentAtomSize_ = 1;

    uint64_t size_ = 0;
    bool direct_ = false;
    bool directCoherent_ = false;
    uint32_t memoryTypeIndex_ = UINT32_MAX;

    std::vector<Segment> segments_;

    VkBuffer stagingBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory_ = VK_NULL_HANDLE;
    VkDeviceSize stagingAllocationSize_ = 0;
    VkDeviceSize stagingSize_ = 0;
    void* stagingMapped_ = nullptr;
    bool stagingCoherent_ = false;

    std::wstring infoText_;
};

}  // namespace grd
