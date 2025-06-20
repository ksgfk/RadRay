#include "vulkan_helper.h"

namespace radray::render::vulkan {

uint64_t GetPhysicalDeviceMemoryAllSize(const VkPhysicalDeviceMemoryProperties& memory, VkMemoryHeapFlags heapFlags) noexcept {
    uint64_t total = 0;
    for (uint32_t i = 0; i < memory.memoryHeapCount; ++i) {
        if ((memory.memoryHeaps[i].flags & heapFlags) == heapFlags) {
            total += memory.memoryHeaps[i].size;
        }
    }
    return total;
}

VkQueueFlags MapType(QueueType v) noexcept {
    switch (v) {
        case QueueType::Direct: return VK_QUEUE_GRAPHICS_BIT;
        case QueueType::Compute: return VK_QUEUE_COMPUTE_BIT;
        case QueueType::Copy: return VK_QUEUE_TRANSFER_BIT;
        default: return 0;
    }
}

std::string_view FormatVkDebugUtilsMessageTypeFlagsEXT(VkDebugUtilsMessageTypeFlagsEXT v) noexcept {
    if (v & VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT) {
        return "General";
    } else if (v & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) {
        return "Validation";
    } else if (v & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) {
        return "Performance";
    } else if (v & VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT) {
        return "DeviceAddressBinding";
    } else {
        return "Unknown";
    }
}

std::string_view FormatVkQueueFlags(VkQueueFlags v) noexcept {
    if (v & VK_QUEUE_GRAPHICS_BIT) {
        return "Graphics";
    } else if (v & VK_QUEUE_COMPUTE_BIT) {
        return "Compute";
    } else if (v & VK_QUEUE_TRANSFER_BIT) {
        return "Transfer";
    } else if (v & VK_QUEUE_SPARSE_BINDING_BIT) {
        return "SparseBinding";
    } else {
        return "Unknown";
    }
}

}  // namespace radray::render::vulkan
