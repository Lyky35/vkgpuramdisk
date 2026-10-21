#include "gpu/vulkan_drive.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <iterator>
#include <stdexcept>

#include "core/log.h"
#include "core/util.h"

namespace grd {
namespace {

constexpr uint64_t kMiB = 1024ull * 1024ull;
constexpr uint64_t kGpuChunk = 256ull * kMiB;
constexpr uint64_t kStagingSize = 4ull * kMiB;
constexpr uint64_t kFenceTimeout = 60ull * 1000ull * 1000ull * 1000ull;

const char* vulkanResultName(VkResult result) {
    switch (result) {
        case VK_SUCCESS:
            return "VK_SUCCESS";
        case VK_ERROR_OUT_OF_HOST_MEMORY:
            return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:
            return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED:
            return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST:
            return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED:
            return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_TOO_MANY_OBJECTS:
            return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_TIMEOUT:
            return "VK_TIMEOUT";
        default:
            return "Vulkan error";
    }
}

std::runtime_error vulkanFailure(const char* operation, VkResult result) {
    return std::runtime_error(std::format("{}: {} ({})", operation,
                                          vulkanResultName(result),
                                          static_cast<int>(result)));
}

bool environmentFlag(const wchar_t* name) {
    wchar_t value[8] = {};
    return GetEnvironmentVariableW(name, value,
                                   static_cast<DWORD>(std::size(value))) > 0;
}

}  // namespace

std::unique_ptr<VulkanDrive> VulkanDrive::create(VulkanApi& api,
                                                 VkPhysicalDevice physicalDevice,
                                                 const GpuDeviceInfo& info,
                                                 uint64_t sizeBytes,
                                                 std::wstring& error) {
    auto drive =
        std::unique_ptr<VulkanDrive>(new VulkanDrive(api, physicalDevice, info));
    try {
        drive->initialize(sizeBytes);
    } catch (const std::exception& ex) {
        error = widen(ex.what());
        Log::error(L"GPU memory allocation failed: " + error);
        return nullptr;
    } catch (...) {
        error = L"Unknown error while allocating GPU memory.";
        return nullptr;
    }
    error.clear();
    return drive;
}

VulkanDrive::VulkanDrive(VulkanApi& api, VkPhysicalDevice physicalDevice,
                         const GpuDeviceInfo& info)
    : api_(api), physicalDevice_(physicalDevice), info_(info) {}

VulkanDrive::~VulkanDrive() {
    cleanup();
}

void VulkanDrive::initialize(uint64_t sizeBytes) {
    if (sizeBytes == 0) {
        throw std::runtime_error("The drive size must be greater than zero.");
    }
    if (sizeBytes > UINT64_MAX - 4095) {
        throw std::runtime_error("The requested drive size is too large.");
    }

    // Sector-sized alignment keeps every ImDisk transfer and every image copy
    // on a clean boundary.
    size_ = (sizeBytes + 4095) & ~uint64_t{4095};

    api_.GetPhysicalDeviceProperties(physicalDevice_, &properties_);
    api_.GetPhysicalDeviceMemoryProperties(physicalDevice_, &memoryProperties_);

    nonCoherentAtomSize_ = properties_.limits.nonCoherentAtomSize;
    if (nonCoherentAtomSize_ == 0) {
        nonCoherentAtomSize_ = 1;
    }

    createLogicalDevice();
    createCommandObjects();
    allocateStorage(size_);
    configureReadPath();

    if (!direct_ || stagedReads_) {
        allocateStagingBuffer();
    }

    zeroStorage();

    const wchar_t* mode = !direct_        ? L"staged over PCIe"
                          : stagedReads_ ? L"direct writes, staged reads"
                                         : L"direct mapped";
    infoText_ = std::format(L"{} ({}), {} MiB, {}", info_.name,
                            info_.typeName(), size_ / kMiB, mode);
    Log::info(L"Allocated GPU drive: " + infoText_);
}

void VulkanDrive::createLogicalDevice() {
    uint32_t familyCount = 0;
    api_.GetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &familyCount,
                                                nullptr);
    if (familyCount == 0) {
        throw std::runtime_error("The Vulkan device has no queue families.");
    }

    std::vector<VkQueueFamilyProperties> families(familyCount);
    api_.GetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &familyCount,
                                                families.data());

    uint32_t selected = UINT32_MAX;
    for (uint32_t i = 0; i < familyCount; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_TRANSFER_BIT) != 0 &&
            families[i].queueCount > 0) {
            selected = i;
            break;
        }
    }
    if (selected == UINT32_MAX) {
        throw std::runtime_error(
            "The Vulkan device has no queue that supports transfers.");
    }

    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queueInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = selected,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    const VkDeviceCreateInfo createInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queueInfo,
        .enabledLayerCount = 0,
        .ppEnabledLayerNames = nullptr,
        .enabledExtensionCount = 0,
        .ppEnabledExtensionNames = nullptr,
        .pEnabledFeatures = nullptr,
    };

    const VkResult result = api_.CreateDevice(physicalDevice_, &createInfo,
                                              nullptr, &device_);
    if (result != VK_SUCCESS) {
        throw vulkanFailure("vkCreateDevice", result);
    }

    api_.GetDeviceQueue(device_, selected, 0, &queue_);
    queueFamily_ = selected;
}

void VulkanDrive::createCommandObjects() {
    if (queueFamily_ == UINT32_MAX) {
        throw std::runtime_error("No transfer queue family was selected.");
    }

    const VkCommandPoolCreateInfo poolInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = queueFamily_,
    };

    VkResult result =
        api_.CreateCommandPool(device_, &poolInfo, nullptr, &commandPool_);
    if (result != VK_SUCCESS) {
        throw vulkanFailure("vkCreateCommandPool", result);
    }

    const VkCommandBufferAllocateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = commandPool_,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    result = api_.AllocateCommandBuffers(device_, &bufferInfo, &commandBuffer_);
    if (result != VK_SUCCESS) {
        throw vulkanFailure("vkAllocateCommandBuffers", result);
    }

    const VkFenceCreateInfo fenceInfo = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = 0,
    };
    result = api_.CreateFence(device_, &fenceInfo, nullptr, &fence_);
    if (result != VK_SUCCESS) {
        throw vulkanFailure("vkCreateFence", result);
    }
}

uint32_t VulkanDrive::chooseMemoryType(uint32_t allowedBits,
                                       bool requireDeviceLocal) {
    uint32_t best = UINT32_MAX;
    int bestScore = -1;

    for (uint32_t i = 0; i < memoryProperties_.memoryTypeCount; ++i) {
        if ((allowedBits & (1u << i)) == 0) {
            continue;
        }

        const VkMemoryPropertyFlags flags = memoryProperties_.memoryTypes[i].propertyFlags;
        if ((flags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) != 0) {
            continue;
        }
        if (requireDeviceLocal &&
            (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0) {
            continue;
        }

        int score = 0;
        if ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
            score += 1000;
        }
        if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
            score += 2000;
        }
        if ((flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0) {
            score += 500;
        }
        if ((flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0) {
            score += 50;
        }

        if (score > bestScore) {
            bestScore = score;
            best = i;
        }
    }

    return best;
}

uint32_t VulkanDrive::chooseStagingMemoryType(uint32_t allowedBits) {
    uint32_t best = UINT32_MAX;
    int bestScore = -1;

    for (uint32_t i = 0; i < memoryProperties_.memoryTypeCount; ++i) {
        if ((allowedBits & (1u << i)) == 0) {
            continue;
        }

        const VkMemoryPropertyFlags flags = memoryProperties_.memoryTypes[i].propertyFlags;
        if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) {
            continue;
        }

        int score = 0;
        if ((flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0) {
            score += 1000;
        }
        if ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0) {
            score += 500;
        }
        if ((flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0) {
            score += 100;
        }

        if (score > bestScore) {
            bestScore = score;
            best = i;
        }
    }

    return best;
}

void VulkanDrive::allocateStorage(uint64_t sizeBytes) {
    const bool forceStaging = environmentFlag(L"GPURAMDRIVE_FORCE_STAGING");

    uint64_t remaining = sizeBytes;
    uint64_t offset = 0;
    uint64_t chunk = std::min(remaining, kGpuChunk);

    while (remaining > 0) {
        bool allocated = false;

        while (!allocated) {
            const VkBufferCreateInfo bufferInfo = {
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .size = chunk,
                .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .queueFamilyIndexCount = 0,
                .pQueueFamilyIndices = nullptr,
            };

            VkBuffer buffer = VK_NULL_HANDLE;
            VkResult result =
                api_.CreateBuffer(device_, &bufferInfo, nullptr, &buffer);
            if (result != VK_SUCCESS) {
                throw vulkanFailure("vkCreateBuffer", result);
            }

            VkMemoryRequirements requirements = {};
            api_.GetBufferMemoryRequirements(device_, buffer, &requirements);

            if (memoryTypeIndex_ == UINT32_MAX) {
                memoryTypeIndex_ = chooseMemoryType(requirements.memoryTypeBits, true);
                if (memoryTypeIndex_ == UINT32_MAX) {
                    api_.DestroyBuffer(device_, buffer, nullptr);
                    throw std::runtime_error(
                        "No device-local Vulkan memory type can back the drive.");
                }
                const VkMemoryPropertyFlags flags =
                    memoryProperties_.memoryTypes[memoryTypeIndex_].propertyFlags;
                direct_ = !forceStaging &&
                          (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
                directCoherent_ =
                    (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
            } else if ((requirements.memoryTypeBits & (1u << memoryTypeIndex_)) == 0) {
                api_.DestroyBuffer(device_, buffer, nullptr);
                throw std::runtime_error(
                    "The Vulkan device requires inconsistent memory types for "
                    "the drive's buffers.");
            }

            const VkMemoryAllocateInfo allocateInfo = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .allocationSize = requirements.size,
                .memoryTypeIndex = memoryTypeIndex_,
            };

            VkDeviceMemory memory = VK_NULL_HANDLE;
            result = api_.AllocateMemory(device_, &allocateInfo, nullptr, &memory);
            if (result != VK_SUCCESS) {
                api_.DestroyBuffer(device_, buffer, nullptr);

                const bool canShrink =
                    (result == VK_ERROR_OUT_OF_DEVICE_MEMORY ||
                     result == VK_ERROR_OUT_OF_HOST_MEMORY) &&
                    chunk > kMiB;
                if (canShrink) {
                    chunk /= 2;
                    if (chunk < kMiB) {
                        chunk = kMiB;
                    }
                    Log::warn(std::format(
                        L"GPU allocation of {} MiB failed; retrying with {} MiB.",
                        chunk * 2 / kMiB, chunk / kMiB));
                    continue;
                }

                if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
                    throw std::runtime_error(std::format(
                        "Not enough GPU memory: requested {} MiB, allocated {} "
                        "MiB before the device ran out.",
                        sizeBytes / kMiB, offset / kMiB));
                }
                throw vulkanFailure("vkAllocateMemory", result);
            }

            result = api_.BindBufferMemory(device_, buffer, memory, 0);
            if (result != VK_SUCCESS) {
                api_.FreeMemory(device_, memory, nullptr);
                api_.DestroyBuffer(device_, buffer, nullptr);
                throw vulkanFailure("vkBindBufferMemory", result);
            }

            void* mapped = nullptr;
            if (direct_) {
                result = api_.MapMemory(device_, memory, 0, VK_WHOLE_SIZE, 0,
                                        &mapped);
                if (result != VK_SUCCESS) {
                    api_.FreeMemory(device_, memory, nullptr);
                    api_.DestroyBuffer(device_, buffer, nullptr);
                    throw vulkanFailure("vkMapMemory", result);
                }
            }

            segments_.push_back(Segment{
                .buffer = buffer,
                .memory = memory,
                .allocationSize = requirements.size,
                .logicalSize = chunk,
                .offset = offset,
                .mapped = mapped,
                .coherent = directCoherent_,
            });

            Log::debug(std::format(
                L"GPU segment {}: {} MiB at offset {} MiB{}", segments_.size() - 1,
                chunk / kMiB, offset / kMiB,
                direct_ ? L" (mapped)" : L" (device-local)"));

            offset += chunk;
            remaining -= chunk;
            chunk = std::min(remaining, kGpuChunk);
            allocated = true;
        }
    }
}

void VulkanDrive::configureReadPath() {
    if (!direct_) {
        stagedReads_ = false;
        return;
    }

    if (environmentFlag(L"GPURAMDRIVE_STAGED_READS")) {
        stagedReads_ = true;
    } else if (environmentFlag(L"GPURAMDRIVE_DIRECT_READS")) {
        stagedReads_ = false;
    } else {
        stagedReads_ =
            info_.type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    }

    Log::debug(std::format(L"Direct-mapped reads: {}",
                           stagedReads_ ? L"staged via copy engine"
                                        : L"CPU memcpy from BAR"));
}

void VulkanDrive::allocateStagingBuffer() {
    stagingSize_ = std::min(size_, kStagingSize);
    if (stagingSize_ == 0) {
        stagingSize_ = 4096;
    }

    const VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = stagingSize_,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };

    VkResult result =
        api_.CreateBuffer(device_, &bufferInfo, nullptr, &stagingBuffer_);
    if (result != VK_SUCCESS) {
        throw vulkanFailure("vkCreateBuffer (staging)", result);
    }

    VkMemoryRequirements requirements = {};
    api_.GetBufferMemoryRequirements(device_, stagingBuffer_, &requirements);

    const uint32_t type = chooseStagingMemoryType(requirements.memoryTypeBits);
    if (type == UINT32_MAX) {
        throw std::runtime_error(
            "The Vulkan device has no host-visible memory for a staging buffer.");
    }

    const VkMemoryAllocateInfo allocateInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = type,
    };
    result = api_.AllocateMemory(device_, &allocateInfo, nullptr, &stagingMemory_);
    if (result != VK_SUCCESS) {
        throw vulkanFailure("vkAllocateMemory (staging)", result);
    }

    result = api_.BindBufferMemory(device_, stagingBuffer_, stagingMemory_, 0);
    if (result != VK_SUCCESS) {
        throw vulkanFailure("vkBindBufferMemory (staging)", result);
    }

    result = api_.MapMemory(device_, stagingMemory_, 0, VK_WHOLE_SIZE, 0,
                            &stagingMapped_);
    if (result != VK_SUCCESS) {
        throw vulkanFailure("vkMapMemory (staging)", result);
    }

    stagingAllocationSize_ = requirements.size;
    stagingCoherent_ =
        (memoryProperties_.memoryTypes[type].propertyFlags &
         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
}

void VulkanDrive::zeroStorage() {
    if (direct_) {
        for (const Segment& segment : segments_) {
            std::memset(segment.mapped, 0, static_cast<size_t>(segment.logicalSize));
            flushRange(segment, 0, segment.logicalSize);
        }
        return;
    }

    for (const Segment& segment : segments_) {
        fillSegment(segment);
    }
}

void VulkanDrive::fillSegment(const Segment& segment) {
    const VkCommandBufferBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };

    VkResult result = api_.BeginCommandBuffer(commandBuffer_, &beginInfo);
    if (result != VK_SUCCESS) {
        throw vulkanFailure("vkBeginCommandBuffer", result);
    }

    api_.CmdFillBuffer(commandBuffer_, segment.buffer, 0, VK_WHOLE_SIZE, 0);

    VkMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
    };
    api_.CmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0,
                            nullptr, 0, nullptr);

    result = api_.EndCommandBuffer(commandBuffer_);
    if (result != VK_SUCCESS) {
        throw vulkanFailure("vkEndCommandBuffer", result);
    }

    const VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 0,
        .pWaitSemaphores = nullptr,
        .pWaitDstStageMask = nullptr,
        .commandBufferCount = 1,
        .pCommandBuffers = &commandBuffer_,
        .signalSemaphoreCount = 0,
        .pSignalSemaphores = nullptr,
    };

    api_.ResetFences(device_, 1, &fence_);
    result = api_.QueueSubmit(queue_, 1, &submitInfo, fence_);
    if (result != VK_SUCCESS) {
        throw vulkanFailure("vkQueueSubmit (zero fill)", result);
    }
    waitForFence();
}

void VulkanDrive::submitCopy(VkBuffer source, VkDeviceSize sourceOffset,
                             VkBuffer dest, VkDeviceSize destOffset,
                             VkDeviceSize size, bool transferToDevice) {
    (void)transferToDevice;

    const VkCommandBufferBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };

    VkResult result = api_.BeginCommandBuffer(commandBuffer_, &beginInfo);
    if (result != VK_SUCCESS) {
        throw vulkanFailure("vkBeginCommandBuffer", result);
    }

    VkMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
    };
    api_.CmdPipelineBarrier(commandBuffer_,
                            VK_PIPELINE_STAGE_HOST_BIT |
                                VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0,
                            nullptr, 0, nullptr);

    const VkBufferCopy region = {
        .srcOffset = sourceOffset,
        .dstOffset = destOffset,
        .size = size,
    };
    api_.CmdCopyBuffer(commandBuffer_, source, dest, 1, &region);

    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    api_.CmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0,
                            nullptr, 0, nullptr);

    result = api_.EndCommandBuffer(commandBuffer_);
    if (result != VK_SUCCESS) {
        throw vulkanFailure("vkEndCommandBuffer", result);
    }

    const VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 0,
        .pWaitSemaphores = nullptr,
        .pWaitDstStageMask = nullptr,
        .commandBufferCount = 1,
        .pCommandBuffers = &commandBuffer_,
        .signalSemaphoreCount = 0,
        .pSignalSemaphores = nullptr,
    };

    api_.ResetFences(device_, 1, &fence_);
    result = api_.QueueSubmit(queue_, 1, &submitInfo, fence_);
    if (result != VK_SUCCESS) {
        throw vulkanFailure("vkQueueSubmit", result);
    }
    waitForFence();
}

void VulkanDrive::waitForFence() {
    const VkResult result =
        api_.WaitForFences(device_, 1, &fence_, VK_TRUE, kFenceTimeout);
    if (result == VK_TIMEOUT) {
        throw std::runtime_error(
            "GPU transfer timed out. The display driver may have reset the GPU.");
    }
    if (result != VK_SUCCESS) {
        throw vulkanFailure("vkWaitForFences", result);
    }
    api_.ResetFences(device_, 1, &fence_);
}

VulkanDrive::Segment& VulkanDrive::locate(uint64_t offset, uint64_t& localOffset,
                                          uint64_t& available) {
    if (offset >= size_) {
        throw std::out_of_range("GPU drive read or write is past the end.");
    }

    size_t low = 0;
    size_t high = segments_.size();
    while (low < high) {
        const size_t middle = (low + high) / 2;
        if (segments_[middle].offset <= offset) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (low == 0) {
        throw std::out_of_range("GPU drive segment lookup failed.");
    }

    Segment& segment = segments_[low - 1];
    localOffset = offset - segment.offset;
    available = segment.logicalSize - localOffset;
    return segment;
}

void VulkanDrive::read(uint64_t offset, void* destination, size_t length) {
    if (length == 0) {
        return;
    }
    if (offset > size_ || length > size_ - offset) {
        throw std::out_of_range("GPU drive read is out of range.");
    }

    auto* out = static_cast<uint8_t*>(destination);
    uint64_t position = offset;

    while (length > 0) {
        uint64_t local = 0;
        uint64_t available = 0;
        Segment& segment = locate(position, local, available);

        const bool directRead = direct_ && !stagedReads_;
        uint64_t chunk = std::min<uint64_t>(length, available);
        if (!directRead) {
            chunk = std::min<uint64_t>(chunk, stagingSize_);
        }

        if (directRead) {
            invalidateRange(segment, local, chunk);
            std::memcpy(out, static_cast<uint8_t*>(segment.mapped) + local,
                        static_cast<size_t>(chunk));
        } else {
            submitCopy(segment.buffer, local, stagingBuffer_, 0, chunk, false);
            invalidateStaging(0, chunk);
            std::memcpy(out, stagingMapped_, static_cast<size_t>(chunk));
        }

        out += chunk;
        position += chunk;
        length -= static_cast<size_t>(chunk);
    }
}

void VulkanDrive::write(uint64_t offset, const void* source, size_t length) {
    if (length == 0) {
        return;
    }
    if (offset > size_ || length > size_ - offset) {
        throw std::out_of_range("GPU drive write is out of range.");
    }

    const auto* in = static_cast<const uint8_t*>(source);
    uint64_t position = offset;

    while (length > 0) {
        uint64_t local = 0;
        uint64_t available = 0;
        Segment& segment = locate(position, local, available);

        uint64_t chunk = std::min<uint64_t>(length, available);
        if (!direct_) {
            chunk = std::min<uint64_t>(chunk, stagingSize_);
        }

        if (direct_) {
            std::memcpy(static_cast<uint8_t*>(segment.mapped) + local, in,
                        static_cast<size_t>(chunk));
            flushRange(segment, local, chunk);
        } else {
            std::memcpy(stagingMapped_, in, static_cast<size_t>(chunk));
            flushStaging(0, chunk);
            submitCopy(stagingBuffer_, 0, segment.buffer, local, chunk, true);
        }

        in += chunk;
        position += chunk;
        length -= static_cast<size_t>(chunk);
    }
}

void VulkanDrive::mappedMemoryBarrier(VkDeviceMemory memory, uint64_t offset,
                                      uint64_t length, bool flush) {
    void* mapped = nullptr;
    VkDeviceSize allocationSize = 0;
    bool coherent = true;

    if (memory == stagingMemory_) {
        mapped = stagingMapped_;
        allocationSize = stagingAllocationSize_;
        coherent = stagingCoherent_;
    } else {
        for (const Segment& segment : segments_) {
            if (segment.memory == memory) {
                mapped = segment.mapped;
                allocationSize = segment.allocationSize;
                coherent = segment.coherent;
                break;
            }
        }
    }

    if (mapped == nullptr || coherent) {
        return;
    }

    VkDeviceSize atom = nonCoherentAtomSize_;
    if (atom == 0) {
        atom = 1;
    }

    const uint64_t start = offset & ~(static_cast<uint64_t>(atom) - 1);
    uint64_t end = (offset + length + atom - 1) & ~(static_cast<uint64_t>(atom) - 1);
    if (end > allocationSize) {
        end = allocationSize;
    }
    if (end <= start) {
        return;
    }

    if (length == 0) {
        return;
    }

    const VkMappedMemoryRange range = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = memory,
        .offset = start,
        .size = end - start,
    };

    const VkResult result =
        flush ? api_.FlushMappedMemoryRanges(device_, 1, &range)
              : api_.InvalidateMappedMemoryRanges(device_, 1, &range);
    if (result != VK_SUCCESS) {
        throw vulkanFailure(flush ? "vkFlushMappedMemoryRanges"
                                  : "vkInvalidateMappedMemoryRanges",
                            result);
    }
}

void VulkanDrive::flushRange(const Segment& segment, uint64_t offset,
                             uint64_t length) {
    mappedMemoryBarrier(segment.memory, offset, length, true);
}

void VulkanDrive::invalidateRange(const Segment& segment, uint64_t offset,
                                  uint64_t length) {
    mappedMemoryBarrier(segment.memory, offset, length, false);
}

void VulkanDrive::flushStaging(uint64_t offset, uint64_t length) {
    mappedMemoryBarrier(stagingMemory_, offset, length, true);
}

void VulkanDrive::invalidateStaging(uint64_t offset, uint64_t length) {
    mappedMemoryBarrier(stagingMemory_, offset, length, false);
}

void VulkanDrive::cleanup() {
    if (device_ != VK_NULL_HANDLE && api_.DeviceWaitIdle != nullptr) {
        api_.DeviceWaitIdle(device_);
    }

    if (stagingMapped_ != nullptr && api_.UnmapMemory != nullptr) {
        api_.UnmapMemory(device_, stagingMemory_);
        stagingMapped_ = nullptr;
    }
    if (stagingBuffer_ != VK_NULL_HANDLE && api_.DestroyBuffer != nullptr) {
        api_.DestroyBuffer(device_, stagingBuffer_, nullptr);
        stagingBuffer_ = VK_NULL_HANDLE;
    }
    if (stagingMemory_ != VK_NULL_HANDLE && api_.FreeMemory != nullptr) {
        api_.FreeMemory(device_, stagingMemory_, nullptr);
        stagingMemory_ = VK_NULL_HANDLE;
    }

    for (Segment& segment : segments_) {
        if (segment.mapped != nullptr && api_.UnmapMemory != nullptr) {
            api_.UnmapMemory(device_, segment.memory);
            segment.mapped = nullptr;
        }
        if (segment.buffer != VK_NULL_HANDLE && api_.DestroyBuffer != nullptr) {
            api_.DestroyBuffer(device_, segment.buffer, nullptr);
            segment.buffer = VK_NULL_HANDLE;
        }
        if (segment.memory != VK_NULL_HANDLE && api_.FreeMemory != nullptr) {
            api_.FreeMemory(device_, segment.memory, nullptr);
            segment.memory = VK_NULL_HANDLE;
        }
    }
    segments_.clear();

    if (fence_ != VK_NULL_HANDLE && api_.DestroyFence != nullptr) {
        api_.DestroyFence(device_, fence_, nullptr);
        fence_ = VK_NULL_HANDLE;
    }
    if (commandPool_ != VK_NULL_HANDLE && api_.DestroyCommandPool != nullptr) {
        api_.DestroyCommandPool(device_, commandPool_, nullptr);
        commandPool_ = VK_NULL_HANDLE;
    }
    if (device_ != VK_NULL_HANDLE && api_.DestroyDevice != nullptr) {
        api_.DestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
}

}  // namespace grd
