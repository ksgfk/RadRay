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

std::string_view formatVkDebugMsgType(VkDebugUtilsMessageTypeFlagsEXT v) noexcept {
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

}  // namespace radray::render::vulkan
