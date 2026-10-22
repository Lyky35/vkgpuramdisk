#pragma once

#include <windows.h>

#include <vulkan/vulkan.h>

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "gpu/vulkan_api.h"
#include "gpu/vulkan_runtime.h"

namespace grd {

// A block of memory that lives on a Vulkan device and is exposed as a flat,
// byte-addressable range. On a discrete GPU the memory is usually not host
// visible, so reads and writes are staged through a small host-visible buffer.
// On an integrated GPU (or a discrete GPU with resizable BAR) the device-local
// memory is mapped directly. Directly mapped VRAM is useful for CPU writes but
// is often extremely slow for CPU reads, so discrete GPUs read through the GPU
// copy engine instead.
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

    struct IoContext;
    class ContextLease;

    void initialize(uint64_t sizeBytes);
    void cleanup();

    void createLogicalDevice();
    void createCommandObjects();
    void allocateStorage(uint64_t sizeBytes);
    void allocateStagingBuffers();
    void configureReadPath();
    void zeroStorage();

    IoContext& acquireContext();
    void releaseContext(IoContext& context);

    uint32_t chooseMemoryType(uint32_t allowedBits, bool requireDeviceLocal);
    uint32_t chooseStagingMemoryType(uint32_t allowedBits);

    Segment& locate(uint64_t offset, uint64_t& localOffset,
                    uint64_t& available);

    void fillSegment(IoContext& context, const Segment& segment);
    void submitCopy(IoContext& context, VkBuffer source,
                    VkDeviceSize sourceOffset, VkBuffer dest,
                    VkDeviceSize destOffset, VkDeviceSize size,
                    bool transferToDevice);
    void waitForFence(IoContext& context);
    void flushRange(const Segment& segment, uint64_t offset, uint64_t length);
    void invalidateRange(const Segment& segment, uint64_t offset,
                         uint64_t length);
    void flushStaging(IoContext& context, uint64_t offset, uint64_t length);
    void invalidateStaging(IoContext& context, uint64_t offset,
                           uint64_t length);
    void mappedMemoryBarrier(VkDeviceMemory memory, void* mapped,
                             VkDeviceSize allocationSize, bool coherent,
                             uint64_t offset, uint64_t length, bool flush);

    VulkanApi& api_;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    GpuDeviceInfo info_;

    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = UINT32_MAX;

    VkPhysicalDeviceProperties properties_ = {};
    VkPhysicalDeviceMemoryProperties memoryProperties_ = {};
    VkDeviceSize nonCoherentAtomSize_ = 1;

    uint64_t size_ = 0;
    bool direct_ = false;
    bool directCoherent_ = false;
    bool stagedReads_ = false;
    uint32_t memoryTypeIndex_ = UINT32_MAX;

    std::vector<Segment> segments_;

    std::vector<std::unique_ptr<IoContext>> contexts_;
    std::mutex contextMutex_;
    std::condition_variable contextAvailable_;
    std::mutex queueMutex_;
    std::mutex flushMutex_;
    VkDeviceSize stagingSize_ = 0;

    std::wstring infoText_;
};

}  // namespace grd
