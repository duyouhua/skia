/*
 * Copyright 2018 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "GrVkAMDMemoryAllocator.h"

#include "vk/GrVkInterface.h"
#include "GrVkUtil.h"

GrVkAMDMemoryAllocator::GrVkAMDMemoryAllocator(VkPhysicalDevice physicalDevice,
                                               VkDevice device,
                                               sk_sp<const GrVkInterface> interface)
        : fAllocator(VK_NULL_HANDLE)
        , fInterface(std::move(interface))
        , fDevice(device) {
#define GR_COPY_FUNCTION(NAME) functions.vk##NAME = fInterface->fFunctions.f##NAME;

    VmaVulkanFunctions functions;
    GR_COPY_FUNCTION(GetPhysicalDeviceProperties);
    GR_COPY_FUNCTION(GetPhysicalDeviceMemoryProperties);
    GR_COPY_FUNCTION(AllocateMemory);
    GR_COPY_FUNCTION(FreeMemory);
    GR_COPY_FUNCTION(MapMemory);
    GR_COPY_FUNCTION(UnmapMemory);
    GR_COPY_FUNCTION(BindBufferMemory);
    GR_COPY_FUNCTION(BindImageMemory);
    GR_COPY_FUNCTION(GetBufferMemoryRequirements);
    GR_COPY_FUNCTION(GetImageMemoryRequirements);
    GR_COPY_FUNCTION(CreateBuffer);
    GR_COPY_FUNCTION(DestroyBuffer);
    GR_COPY_FUNCTION(CreateImage);
    GR_COPY_FUNCTION(DestroyImage);

    // Skia current doesn't support VK_KHR_dedicated_allocation
    functions.vkGetBufferMemoryRequirements2KHR = nullptr;
    functions.vkGetImageMemoryRequirements2KHR = nullptr;

    VmaAllocatorCreateInfo info;
    info.flags = 0;
    info.physicalDevice = physicalDevice;
    info.device = device;
    info.preferredLargeHeapBlockSize = 0;
    info.pAllocationCallbacks = nullptr;
    info.pDeviceMemoryCallbacks = nullptr;
    info.frameInUseCount = 0;
    info.pHeapSizeLimit = nullptr;
    info.pVulkanFunctions = &functions;

    vmaCreateAllocator(&info, &fAllocator);
}

GrVkAMDMemoryAllocator::~GrVkAMDMemoryAllocator() {
    vmaDestroyAllocator(fAllocator);
    fAllocator = VK_NULL_HANDLE;
}

bool GrVkAMDMemoryAllocator::allocateMemoryForImage(VkImage image, AllocationPropertyFlags flags,
                                                    GrVkBackendMemory* backendMemory) {
    VmaAllocationCreateInfo info;
    info.flags = 0;
    info.usage = VMA_MEMORY_USAGE_UNKNOWN;
    info.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    info.preferredFlags = 0;
    info.memoryTypeBits = 0;
    info.pool = VK_NULL_HANDLE;
    info.pUserData = nullptr;

    if (AllocationPropertyFlags::kDedicatedAllocation & flags) {
        info.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    }

    if (AllocationPropertyFlags::kLazyAllocation & flags) {
        info.preferredFlags |= VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
    }

    VmaAllocation allocation;
    VkResult result = vmaAllocateMemoryForImage(fAllocator, image, &info, &allocation, nullptr);
    if (VK_SUCCESS != result) {
        return false;
    }
    *backendMemory = (GrVkBackendMemory)allocation;
    return true;
}

bool GrVkAMDMemoryAllocator::allocateMemoryForBuffer(VkBuffer buffer, BufferUsage usage,
                                                     AllocationPropertyFlags flags,
                                                     GrVkBackendMemory* backendMemory) {
    VmaAllocationCreateInfo info;
    info.flags = 0;
    info.usage = VMA_MEMORY_USAGE_UNKNOWN;
    info.memoryTypeBits = 0;
    info.pool = VK_NULL_HANDLE;
    info.pUserData = nullptr;

    switch (usage) {
        case BufferUsage::kGpuOnly:
            info.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            info.preferredFlags = 0;
            break;
        case BufferUsage::kCpuOnly:
            info.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            info.preferredFlags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
            break;
        case BufferUsage::kCpuWritesGpuReads:
            // First attempt to try memory is also device local
            info.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            info.preferredFlags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
            break;
        case BufferUsage::kGpuWritesCpuReads:
            info.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
            info.preferredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                  VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
            break;
    }

    if (AllocationPropertyFlags::kDedicatedAllocation & flags) {
        info.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    }

    if ((AllocationPropertyFlags::kLazyAllocation & flags) && BufferUsage::kGpuOnly == usage) {
        info.preferredFlags |= VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
    }

    if ((AllocationPropertyFlags::kPersistentlyMapped & flags) && BufferUsage::kGpuOnly != usage) {
        info.flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }

    VmaAllocation allocation;
    VkResult result = vmaAllocateMemoryForBuffer(fAllocator, buffer, &info, &allocation, nullptr);
    if (VK_SUCCESS != result) {
        if (usage == BufferUsage::kCpuWritesGpuReads) {
            // We try again but this time drop the requirement for device local
            info.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
            result = vmaAllocateMemoryForBuffer(fAllocator, buffer, &info, &allocation, nullptr);
        }
    }
    if (VK_SUCCESS != result) {
        return false;
    }
    *backendMemory = (GrVkBackendMemory)allocation;
    return true;
}

void GrVkAMDMemoryAllocator::freeMemory(const GrVkBackendMemory& memoryHandle) {
    const VmaAllocation allocation = (const VmaAllocation)memoryHandle;
    vmaFreeMemory(fAllocator, allocation);
}

void GrVkAMDMemoryAllocator::getAllocInfo(const GrVkBackendMemory& memoryHandle,
                                          GrVkAlloc* alloc) const {
    const VmaAllocation allocation = (const VmaAllocation)memoryHandle;
    VmaAllocationInfo vmaInfo;
    vmaGetAllocationInfo(fAllocator, allocation, &vmaInfo);

    VkMemoryPropertyFlags memFlags;
    vmaGetMemoryTypeProperties(fAllocator, vmaInfo.memoryType, &memFlags);

    uint32_t flags = 0;
    if (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT & memFlags) {
        flags |= GrVkAlloc::kMappable_Flag;
    }
    if (!SkToBool(VK_MEMORY_PROPERTY_HOST_COHERENT_BIT & memFlags)) {
        flags |= GrVkAlloc::kNoncoherent_Flag;
    }

    alloc->fMemory        = vmaInfo.deviceMemory;
    alloc->fOffset        = vmaInfo.offset;
    alloc->fSize          = vmaInfo.size;
    alloc->fFlags         = flags;
    alloc->fBackendMemory = memoryHandle;
}

void* GrVkAMDMemoryAllocator::mapMemory(const GrVkBackendMemory& memoryHandle) {
    const VmaAllocation allocation = (const VmaAllocation)memoryHandle;
    void* mapPtr;
    vmaMapMemory(fAllocator, allocation, &mapPtr);
    return mapPtr;
}

void GrVkAMDMemoryAllocator::unmapMemory(const GrVkBackendMemory& memoryHandle) {
    const VmaAllocation allocation = (const VmaAllocation)memoryHandle;
    vmaUnmapMemory(fAllocator, allocation);
}

void GrVkAMDMemoryAllocator::flushMappedMemory(const GrVkBackendMemory& memoryHandle,
                                               VkDeviceSize offset, VkDeviceSize size) {
    GrVkAlloc info;
    this->getAllocInfo(memoryHandle, &info);

    if (GrVkAlloc::kNoncoherent_Flag & info.fFlags) {
        // We need to store the nonCoherentAtomSize for non-coherent flush/invalidate alignment.
        const VkPhysicalDeviceProperties* physDevProps;
        vmaGetPhysicalDeviceProperties(fAllocator, &physDevProps);
        VkDeviceSize alignment = physDevProps->limits.nonCoherentAtomSize;

        offset = offset + info.fOffset;
        VkDeviceSize offsetDiff = offset & (alignment -1);
        offset = offset - offsetDiff;
        size = (size + alignment - 1) & ~(alignment - 1);
#ifdef SK_DEBUG
        SkASSERT(offset >= info.fOffset);
        SkASSERT(offset + size <= info.fOffset + info.fSize);
        SkASSERT(0 == (offset & (alignment-1)));
        SkASSERT(size > 0);
        SkASSERT(0 == (size & (alignment-1)));
#endif

        VkMappedMemoryRange mappedMemoryRange;
        memset(&mappedMemoryRange, 0, sizeof(VkMappedMemoryRange));
        mappedMemoryRange.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        mappedMemoryRange.memory = info.fMemory;
        mappedMemoryRange.offset = offset;
        mappedMemoryRange.size = size;
        GR_VK_CALL(fInterface, FlushMappedMemoryRanges(fDevice, 1, &mappedMemoryRange));
    }
}

void GrVkAMDMemoryAllocator::invalidateMappedMemory(const GrVkBackendMemory& memoryHandle,
                                                    VkDeviceSize offset, VkDeviceSize size) {
    GrVkAlloc info;
    this->getAllocInfo(memoryHandle, &info);

    if (GrVkAlloc::kNoncoherent_Flag & info.fFlags) {
        // We need to store the nonCoherentAtomSize for non-coherent flush/invalidate alignment.
        const VkPhysicalDeviceProperties* physDevProps;
        vmaGetPhysicalDeviceProperties(fAllocator, &physDevProps);
        VkDeviceSize alignment = physDevProps->limits.nonCoherentAtomSize;

        offset = offset + info.fOffset;
        VkDeviceSize offsetDiff = offset & (alignment -1);
        offset = offset - offsetDiff;
        size = (size + alignment - 1) & ~(alignment - 1);
#ifdef SK_DEBUG
        SkASSERT(offset >= info.fOffset);
        SkASSERT(offset + size <= info.fOffset + info.fSize);
        SkASSERT(0 == (offset & (alignment-1)));
        SkASSERT(size > 0);
        SkASSERT(0 == (size & (alignment-1)));
#endif

        VkMappedMemoryRange mappedMemoryRange;
        memset(&mappedMemoryRange, 0, sizeof(VkMappedMemoryRange));
        mappedMemoryRange.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        mappedMemoryRange.memory = info.fMemory;
        mappedMemoryRange.offset = offset;
        mappedMemoryRange.size = size;
        GR_VK_CALL(fInterface, InvalidateMappedMemoryRanges(fDevice, 1, &mappedMemoryRange));
    }
}

uint64_t GrVkAMDMemoryAllocator::totalUsedMemory() const {
    VmaStats stats;
    vmaCalculateStats(fAllocator, &stats);
    return stats.total.usedBytes;
}

uint64_t GrVkAMDMemoryAllocator::totalAllocatedMemory() const {
    VmaStats stats;
    vmaCalculateStats(fAllocator, &stats);
    return stats.total.usedBytes + stats.total.unusedBytes;
}

