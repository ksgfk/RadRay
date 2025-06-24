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

std::string_view to_string(enum VkPhysicalDeviceType v) noexcept {
    switch (v) {
        case VK_PHYSICAL_DEVICE_TYPE_OTHER: return "Other";
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "Integrated";
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "Discrete";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "Virtual";
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return "CPU";
        default: return "Unknown";
    }
}

std::string_view to_string(VkResult v) noexcept {
    switch (v) {
        case VK_SUCCESS: return "Success";
        case VK_NOT_READY: return "NotReady";
        case VK_TIMEOUT: return "Timeout";
        case VK_EVENT_SET: return "EventSet";
        case VK_EVENT_RESET: return "EventReset";
        case VK_INCOMPLETE: return "Incomplete";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "ErrorOutOfHostMemory";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "ErrorOutOfDeviceMemory";
        case VK_ERROR_INITIALIZATION_FAILED: return "ErrorInitializationFailed";
        case VK_ERROR_DEVICE_LOST: return "ErrorDeviceLost";
        case VK_ERROR_MEMORY_MAP_FAILED: return "ErrorMemoryMapFailed";
        case VK_ERROR_LAYER_NOT_PRESENT: return "ErrorLayerNotPresent";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "ErrorExtensionNotPresent";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "ErrorFeatureNotPresent";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "ErrorIncompatibleDriver";
        case VK_ERROR_TOO_MANY_OBJECTS: return "ErrorTooManyObjects";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "ErrorFormatNotSupported";
        case VK_ERROR_FRAGMENTED_POOL: return "ErrorFragmentedPool";
        case VK_ERROR_UNKNOWN: return "ErrorUnknown";
        case VK_ERROR_OUT_OF_POOL_MEMORY: return "ErrorOutOfPoolMemory";
        case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "ErrorInvalidExternalHandle";
        case VK_ERROR_FRAGMENTATION: return "ErrorFragmentation";
        case VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS: return "ErrorInvalidOpaqueCaptureAddress";
        case VK_PIPELINE_COMPILE_REQUIRED: return "PipelineCompileRequired";
        case VK_ERROR_NOT_PERMITTED: return "ErrorNotPermitted";
        case VK_ERROR_SURFACE_LOST_KHR: return "ErrorSurfaceLostKHR";
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "ErrorNativeWindowInUseKHR";
        case VK_SUBOPTIMAL_KHR: return "SuboptimalKHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "ErrorOutOfDateKHR";
        case VK_ERROR_INCOMPATIBLE_DISPLAY_KHR: return "ErrorIncompatibleDisplayKHR";
        case VK_ERROR_VALIDATION_FAILED_EXT: return "ErrorValidationFailedEXT";
        case VK_ERROR_INVALID_SHADER_NV: return "ErrorInvalidShaderNV";
        case VK_ERROR_IMAGE_USAGE_NOT_SUPPORTED_KHR: return "ErrorImageUsageNotSupportedKHR";
        case VK_ERROR_VIDEO_PICTURE_LAYOUT_NOT_SUPPORTED_KHR: return "ErrorVideoPictureLayoutNotSupportedKHR";
        case VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR: return "ErrorVideoProfileOperationNotSupportedKHR";
        case VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR: return "ErrorVideoProfileFormatNotSupportedKHR";
        case VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR: return "ErrorVideoProfileCodecNotSupportedKHR";
        case VK_ERROR_VIDEO_STD_VERSION_NOT_SUPPORTED_KHR: return "ErrorVideoStdVersionNotSupportedKHR";
        case VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT: return "ErrorInvalidDrmFormatModifierPlaneLayoutEXT";
        case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT: return "ErrorFullScreenExclusiveModeLostEXT";
        case VK_THREAD_IDLE_KHR: return "ThreadIdleKHR";
        case VK_THREAD_DONE_KHR: return "ThreadDoneKHR";
        case VK_OPERATION_DEFERRED_KHR: return "OperationDeferredKHR";
        case VK_OPERATION_NOT_DEFERRED_KHR: return "OperationNotDeferredKHR";
        case VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR: return "ErrorInvalidVideoStdParametersKHR";
        case VK_ERROR_COMPRESSION_EXHAUSTED_EXT: return "ErrorCompressionExhaustedEXT";
        case VK_INCOMPATIBLE_SHADER_BINARY_EXT: return "IncompatibleShaderBinaryEXT";
        case VK_PIPELINE_BINARY_MISSING_KHR: return "PipelineBinaryMissingKHR";
        case VK_ERROR_NOT_ENOUGH_SPACE_KHR: return "ErrorNotEnoughSpaceKHR";
        default: return "Unknown";
    }
}

}  // namespace radray::render::vulkan
