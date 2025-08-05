#include "vulkan_helper.h"

#include <cstring>

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

bool IsValidateExtensions(std::span<const char*> required, std::span<VkExtensionProperties> available) noexcept {
    bool allFound = true;
    for (const auto* extensionName : required) {
        bool found = false;
        for (const auto& availableExtension : available) {
            if (std::strcmp(availableExtension.extensionName, extensionName) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            allFound = false;
        }
    }
    return allFound;
}

bool IsValidateLayers(std::span<const char*> required, std::span<VkLayerProperties> available) noexcept {
    bool allFound = true;
    for (const auto* layerName : required) {
        bool found = false;
        for (const auto& availableLayer : available) {
            if (std::strcmp(availableLayer.layerName, layerName) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            allFound = false;
        }
    }
    return allFound;
}

std::optional<VkSurfaceFormatKHR> SelectSurfaceFormat(VkPhysicalDevice gpu, VkSurfaceKHR surface, std::span<VkFormat> preferred) noexcept {
    vector<VkSurfaceFormatKHR> supported;
    EnumerateVectorFromVkFunc(supported, vkGetPhysicalDeviceSurfaceFormatsKHR, gpu, surface);
    if (supported.empty()) {
        return std::nullopt;
    }

    auto it = std::ranges::find_if(
        supported,
        [&preferred](VkSurfaceFormatKHR surfaceFormat) {
            return std::ranges::any_of(
                preferred,
                [&surfaceFormat](VkFormat format) { return format == surfaceFormat.format; });
        });

    return it != supported.end() ? *it : supported[0];
}

VkImageAspectFlags ImageFormatToAspectFlags(VkFormat v) noexcept {
    switch (v) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_X8_D24_UNORM_PACK32:
        case VK_FORMAT_D32_SFLOAT:
            return VK_IMAGE_ASPECT_DEPTH_BIT;
        case VK_FORMAT_S8_UINT:
            return VK_IMAGE_ASPECT_STENCIL_BIT;
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        default:
            return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

VkAccessFlags BufferUseToAccessFlags(BufferUses v) noexcept {
    VkAccessFlags access = 0;
    if (v.HasFlag(BufferUse::MapRead)) {
        access |= VK_ACCESS_HOST_READ_BIT;
    }
    if (v.HasFlag(BufferUse::MapWrite)) {
        access |= VK_ACCESS_HOST_WRITE_BIT;
    }
    if (v.HasFlag(BufferUse::CopySource)) {
        access |= VK_ACCESS_TRANSFER_READ_BIT;
    }
    if (v.HasFlag(BufferUse::CopyDestination)) {
        access |= VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    if (v.HasFlag(BufferUse::Index)) {
        access |= VK_ACCESS_INDEX_READ_BIT;
    }
    if (v.HasFlag(BufferUse::Vertex)) {
        access |= VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    }
    if (v.HasFlag(BufferUse::CBuffer)) {
        access |= VK_ACCESS_UNIFORM_READ_BIT;
    }
    if (v.HasFlag(BufferUse::UnorderedAccess)) {
        access |= VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    }
    if (v.HasFlag(BufferUse::Indirect)) {
        access |= VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    }
    return access;
}

VkPipelineStageFlags BufferUseToPipelineStageFlags(BufferUses v) noexcept {
    VkPipelineStageFlags stage = 0;
    if (v.HasFlag(BufferUse::MapRead) || v.HasFlag(BufferUse::MapWrite)) {
        stage |= VK_PIPELINE_STAGE_HOST_BIT;
    }
    if (v.HasFlag(BufferUse::CopySource) || v.HasFlag(BufferUse::CopyDestination)) {
        stage |= VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    if (v.HasFlag(BufferUse::Index) || v.HasFlag(BufferUse::Vertex)) {
        stage |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
    }
    if (v.HasFlag(BufferUse::CBuffer) || v.HasFlag(BufferUse::UnorderedAccess)) {
        stage |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                 VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT |
                 VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
                 VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT |
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    }
    if (v.HasFlag(BufferUse::Indirect)) {
        stage |= VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
    }
    return stage;
}

VkAccessFlags TextureUseToAccessFlags(TextureUses v) noexcept {
    VkAccessFlags access = 0;
    if (v.HasFlag(TextureUse::CopySource)) {
        access |= VK_ACCESS_TRANSFER_READ_BIT;
    }
    if (v.HasFlag(TextureUse::CopyDestination)) {
        access |= VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    if (v.HasFlag(TextureUse::Resource)) {
        access |= VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
    }
    if (v.HasFlag(TextureUse::RenderTarget)) {
        access |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    }
    if (v.HasFlag(TextureUse::DepthStencilRead)) {
        access |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    }
    if (v.HasFlag(TextureUse::DepthStencilWrite)) {
        access |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }
    if (v.HasFlag(TextureUse::UnorderedAccess)) {
        access |= VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    }
    return access;
}

VkPipelineStageFlags TextureUseToPipelineStageFlags(TextureUses v, bool isSrc) noexcept {
    VkPipelineStageFlags stage = 0;
    if (v.HasFlag(TextureUse::CopySource) || v.HasFlag(TextureUse::CopyDestination)) {
        stage |= VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    if (v.HasFlag(TextureUse::Resource)) {
        stage |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                 VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT |
                 VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
                 VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT |
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    }
    if (v.HasFlag(TextureUse::RenderTarget)) {
        stage |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    }
    if (v.HasFlag(TextureUse::DepthStencilRead) || v.HasFlag(TextureUse::DepthStencilWrite)) {
        stage |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    }
    if (v.HasFlag(TextureUse::UnorderedAccess)) {
        stage |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                 VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT |
                 VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
                 VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT |
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    }
    if (v.HasFlag(TextureUse::Present)) {
        stage |= isSrc ? (VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)
                       : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    }
    if (v.HasFlag(TextureUse::Uninitialized) || v == TextureUse::UNKNOWN) {
        if (isSrc) {
            stage |= VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        }
    }
    return stage;
}

VkImageLayout TextureUseToLayout(TextureUses v) noexcept {
    switch (v) {
        case TextureUse::Present: return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        case TextureUse::CopySource: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        case TextureUse::CopyDestination: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        case TextureUse::RenderTarget: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        case TextureUse::DepthStencilRead: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        case TextureUse::DepthStencilWrite: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        case TextureUse::Resource: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        case TextureUse::UnorderedAccess: return VK_IMAGE_LAYOUT_GENERAL;
        case TextureUse::UNKNOWN:
        case TextureUse::Uninitialized:
        default: return VK_IMAGE_LAYOUT_UNDEFINED;
    }
}

VkQueueFlags MapType(QueueType v) noexcept {
    switch (v) {
        case QueueType::Direct: return VK_QUEUE_GRAPHICS_BIT;
        case QueueType::Compute: return VK_QUEUE_COMPUTE_BIT;
        case QueueType::Copy: return VK_QUEUE_TRANSFER_BIT;
        default: return 0;
    }
}

VkFormat MapType(TextureFormat v) noexcept {
    switch (v) {
        case TextureFormat::R8_SINT: return VK_FORMAT_R8_SINT;
        case TextureFormat::R8_UINT: return VK_FORMAT_R8_UINT;
        case TextureFormat::R8_SNORM: return VK_FORMAT_R8_SNORM;
        case TextureFormat::R8_UNORM: return VK_FORMAT_R8_UNORM;
        case TextureFormat::R16_SINT: return VK_FORMAT_R16_SINT;
        case TextureFormat::R16_UINT: return VK_FORMAT_R16_UINT;
        case TextureFormat::R16_SNORM: return VK_FORMAT_R16_SNORM;
        case TextureFormat::R16_UNORM: return VK_FORMAT_R16_UNORM;
        case TextureFormat::R16_FLOAT: return VK_FORMAT_R16_SFLOAT;
        case TextureFormat::RG8_SINT: return VK_FORMAT_R8G8_SINT;
        case TextureFormat::RG8_UINT: return VK_FORMAT_R8G8_UINT;
        case TextureFormat::RG8_SNORM: return VK_FORMAT_R8G8_SNORM;
        case TextureFormat::RG8_UNORM: return VK_FORMAT_R8G8_UNORM;
        case TextureFormat::R32_SINT: return VK_FORMAT_R32_SINT;
        case TextureFormat::R32_UINT: return VK_FORMAT_R32_UINT;
        case TextureFormat::R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
        case TextureFormat::RG16_SINT: return VK_FORMAT_R16G16_SINT;
        case TextureFormat::RG16_UINT: return VK_FORMAT_R16G16_UINT;
        case TextureFormat::RG16_SNORM: return VK_FORMAT_R16G16_SNORM;
        case TextureFormat::RG16_UNORM: return VK_FORMAT_R16G16_UNORM;
        case TextureFormat::RG16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
        case TextureFormat::RGBA8_SINT: return VK_FORMAT_R8G8B8A8_SINT;
        case TextureFormat::RGBA8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
        case TextureFormat::RGBA8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
        case TextureFormat::RGBA8_UNORM_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
        case TextureFormat::BGRA8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
        case TextureFormat::BGRA8_UNORM_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
        case TextureFormat::RGB10A2_UINT: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
        case TextureFormat::RGB10A2_UNORM: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case TextureFormat::RG11B10_FLOAT: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        case TextureFormat::RG32_SINT: return VK_FORMAT_R32G32_SINT;
        case TextureFormat::RG32_UINT: return VK_FORMAT_R32G32_UINT;
        case TextureFormat::RG32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
        case TextureFormat::RGBA16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
        case TextureFormat::RGBA16_UINT: return VK_FORMAT_R16G16B16A16_UINT;
        case TextureFormat::RGBA16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
        case TextureFormat::RGBA16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
        case TextureFormat::RGBA16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case TextureFormat::RGBA32_SINT: return VK_FORMAT_R32G32B32A32_SINT;
        case TextureFormat::RGBA32_UINT: return VK_FORMAT_R32G32B32A32_UINT;
        case TextureFormat::RGBA32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case TextureFormat::S8: return VK_FORMAT_S8_UINT;
        case TextureFormat::D16_UNORM: return VK_FORMAT_D16_UNORM;
        case TextureFormat::D32_FLOAT: return VK_FORMAT_D32_SFLOAT;
        case TextureFormat::D24_UNORM_S8_UINT: return VK_FORMAT_D24_UNORM_S8_UINT;
        case TextureFormat::D32_FLOAT_S8_UINT: return VK_FORMAT_D32_SFLOAT_S8_UINT;
        default: return VK_FORMAT_UNDEFINED;
    }
}

VkImageType MapType(TextureDimension v) noexcept {
    switch (v) {
        case TextureDimension::Dim1D: return VK_IMAGE_TYPE_1D;
        case TextureDimension::Dim2D: return VK_IMAGE_TYPE_2D;
        case TextureDimension::Dim3D: return VK_IMAGE_TYPE_3D;
        default: return VK_IMAGE_TYPE_MAX_ENUM;
    }
}

VkSampleCountFlagBits MapSampleCount(uint32_t v) noexcept {
    switch (v) {
        case 1: return VK_SAMPLE_COUNT_1_BIT;
        case 2: return VK_SAMPLE_COUNT_2_BIT;
        case 4: return VK_SAMPLE_COUNT_4_BIT;
        case 8: return VK_SAMPLE_COUNT_8_BIT;
        case 16: return VK_SAMPLE_COUNT_16_BIT;
        case 32: return VK_SAMPLE_COUNT_32_BIT;
        case 64: return VK_SAMPLE_COUNT_64_BIT;
        default: return VK_SAMPLE_COUNT_1_BIT;
    }
}

VkImageViewType MapType(TextureViewDimension v) noexcept {
    switch (v) {
        case TextureViewDimension::Dim1D: return VK_IMAGE_VIEW_TYPE_1D;
        case TextureViewDimension::Dim2D: return VK_IMAGE_VIEW_TYPE_2D;
        case TextureViewDimension::Dim3D: return VK_IMAGE_VIEW_TYPE_3D;
        case TextureViewDimension::Dim1DArray: return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
        case TextureViewDimension::Dim2DArray: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        case TextureViewDimension::Cube: return VK_IMAGE_VIEW_TYPE_CUBE;
        case TextureViewDimension::CubeArray: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        default: return VK_IMAGE_VIEW_TYPE_MAX_ENUM;
    }
}

VkAttachmentLoadOp MapType(LoadAction v) noexcept {
    switch (v) {
        case LoadAction::Load: return VK_ATTACHMENT_LOAD_OP_LOAD;
        case LoadAction::Clear: return VK_ATTACHMENT_LOAD_OP_CLEAR;
        case LoadAction::DontCare: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        default: return VK_ATTACHMENT_LOAD_OP_MAX_ENUM;
    }
}

VkAttachmentStoreOp MapType(StoreAction v) noexcept {
    switch (v) {
        case StoreAction::Store: return VK_ATTACHMENT_STORE_OP_STORE;
        case StoreAction::Discard: return VK_ATTACHMENT_STORE_OP_DONT_CARE;
        default: return VK_ATTACHMENT_STORE_OP_MAX_ENUM;
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
        return "UNKNOWN";
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
        return "UNKNOWN";
    }
}

std::string_view to_string(enum VkPhysicalDeviceType v) noexcept {
    switch (v) {
        case VK_PHYSICAL_DEVICE_TYPE_OTHER: return "Other";
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "Integrated";
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "Discrete";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "Virtual";
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return "CPU";
        default: return "UNKNOWN";
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
        case VK_ERROR_UNKNOWN: return "ErrorUNKNOWN";
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
        default: return "UNKNOWN";
    }
}

std::string_view to_string(VkFormat v) noexcept {
    switch (v) {
        case VK_FORMAT_UNDEFINED: return "UNDEFINED";
        case VK_FORMAT_R4G4_UNORM_PACK8: return "R4G4_UNORM_PACK8";
        case VK_FORMAT_R4G4B4A4_UNORM_PACK16: return "R4G4B4A4_UNORM_PACK16";
        case VK_FORMAT_B4G4R4A4_UNORM_PACK16: return "B4G4R4A4_UNORM_PACK16";
        case VK_FORMAT_R5G6B5_UNORM_PACK16: return "R5G6B5_UNORM_PACK16";
        case VK_FORMAT_B5G6R5_UNORM_PACK16: return "B5G6R5_UNORM_PACK16";
        case VK_FORMAT_R5G5B5A1_UNORM_PACK16: return "R5G5B5A1_UNORM_PACK16";
        case VK_FORMAT_B5G5R5A1_UNORM_PACK16: return "B5G5R5A1_UNORM_PACK16";
        case VK_FORMAT_A1R5G5B5_UNORM_PACK16: return "A1R5G5B5_UNORM_PACK16";
        case VK_FORMAT_R8_UNORM: return "R8_UNORM";
        case VK_FORMAT_R8_SNORM: return "R8_SNORM";
        case VK_FORMAT_R8_USCALED: return "R8_USCALED";
        case VK_FORMAT_R8_SSCALED: return "R8_SSCALED";
        case VK_FORMAT_R8_UINT: return "R8_UINT";
        case VK_FORMAT_R8_SINT: return "R8_SINT";
        case VK_FORMAT_R8_SRGB: return "R8_SRGB";
        case VK_FORMAT_R8G8_UNORM: return "R8G8_UNORM";
        case VK_FORMAT_R8G8_SNORM: return "R8G8_SNORM";
        case VK_FORMAT_R8G8_USCALED: return "R8G8_USCALED";
        case VK_FORMAT_R8G8_SSCALED: return "R8G8_SSCALED";
        case VK_FORMAT_R8G8_UINT: return "R8G8_UINT";
        case VK_FORMAT_R8G8_SINT: return "R8G8_SINT";
        case VK_FORMAT_R8G8_SRGB: return "R8G8_SRGB";
        case VK_FORMAT_R8G8B8_UNORM: return "R8G8B8_UNORM";
        case VK_FORMAT_R8G8B8_SNORM: return "R8G8B8_SNORM";
        case VK_FORMAT_R8G8B8_USCALED: return "R8G8B8_USCALED";
        case VK_FORMAT_R8G8B8_SSCALED: return "R8G8B8_SSCALED";
        case VK_FORMAT_R8G8B8_UINT: return "R8G8B8_UINT";
        case VK_FORMAT_R8G8B8_SINT: return "R8G8B8_SINT";
        case VK_FORMAT_R8G8B8_SRGB: return "R8G8B8_SRGB";
        case VK_FORMAT_B8G8R8_UNORM: return "B8G8R8_UNORM";
        case VK_FORMAT_B8G8R8_SNORM: return "B8G8R8_SNORM";
        case VK_FORMAT_B8G8R8_USCALED: return "B8G8R8_USCALED";
        case VK_FORMAT_B8G8R8_SSCALED: return "B8G8R8_SSCALED";
        case VK_FORMAT_B8G8R8_UINT: return "B8G8R8_UINT";
        case VK_FORMAT_B8G8R8_SINT: return "B8G8R8_SINT";
        case VK_FORMAT_B8G8R8_SRGB: return "B8G8R8_SRGB";
        case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case VK_FORMAT_R8G8B8A8_SNORM: return "R8G8B8A8_SNORM";
        case VK_FORMAT_R8G8B8A8_USCALED: return "R8G8B8A8_USCALED";
        case VK_FORMAT_R8G8B8A8_SSCALED: return "R8G8B8A8_SSCALED";
        case VK_FORMAT_R8G8B8A8_UINT: return "R8G8B8A8_UINT";
        case VK_FORMAT_R8G8B8A8_SINT: return "R8G8B8A8_SINT";
        case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
        case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case VK_FORMAT_B8G8R8A8_SNORM: return "B8G8R8A8_SNORM";
        case VK_FORMAT_B8G8R8A8_USCALED: return "B8G8R8A8_USCALED";
        case VK_FORMAT_B8G8R8A8_SSCALED: return "B8G8R8A8_SSCALED";
        case VK_FORMAT_B8G8R8A8_UINT: return "B8G8R8A8_UINT";
        case VK_FORMAT_B8G8R8A8_SINT: return "B8G8R8A8_SINT";
        case VK_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8_SRGB";
        case VK_FORMAT_A8B8G8R8_UNORM_PACK32: return "A8B8G8R8_UNORM_PACK32";
        case VK_FORMAT_A8B8G8R8_SNORM_PACK32: return "A8B8G8R8_SNORM_PACK32";
        case VK_FORMAT_A8B8G8R8_USCALED_PACK32: return "A8B8G8R8_USCALED_PACK32";
        case VK_FORMAT_A8B8G8R8_SSCALED_PACK32: return "A8B8G8R8_SSCALED_PACK32";
        case VK_FORMAT_A8B8G8R8_UINT_PACK32: return "A8B8G8R8_UINT_PACK32";
        case VK_FORMAT_A8B8G8R8_SINT_PACK32: return "A8B8G8R8_SINT_PACK32";
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return "A8B8G8R8_SRGB_PACK32";
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return "A2R10G10B10_UNORM_PACK32";
        case VK_FORMAT_A2R10G10B10_SNORM_PACK32: return "A2R10G10B10_SNORM_PACK32";
        case VK_FORMAT_A2R10G10B10_USCALED_PACK32: return "A2R10G10B10_USCALED_PACK32";
        case VK_FORMAT_A2R10G10B10_SSCALED_PACK32: return "A2R10G10B10_SSCALED_PACK32";
        case VK_FORMAT_A2R10G10B10_UINT_PACK32: return "A2R10G10B10_UINT_PACK32";
        case VK_FORMAT_A2R10G10B10_SINT_PACK32: return "A2R10G10B10_SINT_PACK32";
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10_UNORM_PACK32";
        case VK_FORMAT_A2B10G10R10_SNORM_PACK32: return "A2B10G10R10_SNORM_PACK32";
        case VK_FORMAT_A2B10G10R10_USCALED_PACK32: return "A2B10G10R10_USCALED_PACK32";
        case VK_FORMAT_A2B10G10R10_SSCALED_PACK32: return "A2B10G10R10_SSCALED_PACK32";
        case VK_FORMAT_A2B10G10R10_UINT_PACK32: return "A2B10G10R10_UINT_PACK32";
        case VK_FORMAT_A2B10G10R10_SINT_PACK32: return "A2B10G10R10_SINT_PACK32";
        case VK_FORMAT_R16_UNORM: return "R16_UNORM";
        case VK_FORMAT_R16_SNORM: return "R16_SNORM";
        case VK_FORMAT_R16_USCALED: return "R16_USCALED";
        case VK_FORMAT_R16_SSCALED: return "R16_SSCALED";
        case VK_FORMAT_R16_UINT: return "R16_UINT";
        case VK_FORMAT_R16_SINT: return "R16_SINT";
        case VK_FORMAT_R16_SFLOAT: return "R16_SFLOAT";
        case VK_FORMAT_R16G16_UNORM: return "R16G16_UNORM";
        case VK_FORMAT_R16G16_SNORM: return "R16G16_SNORM";
        case VK_FORMAT_R16G16_USCALED: return "R16G16_USCALED";
        case VK_FORMAT_R16G16_SSCALED: return "R16G16_SSCALED";
        case VK_FORMAT_R16G16_UINT: return "R16G16_UINT";
        case VK_FORMAT_R16G16_SINT: return "R16G16_SINT";
        case VK_FORMAT_R16G16_SFLOAT: return "R16G16_SFLOAT";
        case VK_FORMAT_R16G16B16_UNORM: return "R16G16B16_UNORM";
        case VK_FORMAT_R16G16B16_SNORM: return "R16G16B16_SNORM";
        case VK_FORMAT_R16G16B16_USCALED: return "R16G16B16_USCALED";
        case VK_FORMAT_R16G16B16_SSCALED: return "R16G16B16_SSCALED";
        case VK_FORMAT_R16G16B16_UINT: return "R16G16B16_UINT";
        case VK_FORMAT_R16G16B16_SINT: return "R16G16B16_SINT";
        case VK_FORMAT_R16G16B16_SFLOAT: return "R16G16B16_SFLOAT";
        case VK_FORMAT_R16G16B16A16_UNORM: return "R16G16B16A16_UNORM";
        case VK_FORMAT_R16G16B16A16_SNORM: return "R16G16B16A16_SNORM";
        case VK_FORMAT_R16G16B16A16_USCALED: return "R16G16B16A16_USCALED";
        case VK_FORMAT_R16G16B16A16_SSCALED: return "R16G16B16A16_SSCALED";
        case VK_FORMAT_R16G16B16A16_UINT: return "R16G16B16A16_UINT";
        case VK_FORMAT_R16G16B16A16_SINT: return "R16G16B16A16_SINT";
        case VK_FORMAT_R16G16B16A16_SFLOAT: return "R16G16B16A16_SFLOAT";
        case VK_FORMAT_R32_UINT: return "R32_UINT";
        case VK_FORMAT_R32_SINT: return "R32_SINT";
        case VK_FORMAT_R32_SFLOAT: return "R32_SFLOAT";
        case VK_FORMAT_R32G32_UINT: return "R32G32_UINT";
        case VK_FORMAT_R32G32_SINT: return "R32G32_SINT";
        case VK_FORMAT_R32G32_SFLOAT: return "R32G32_SFLOAT";
        case VK_FORMAT_R32G32B32_UINT: return "R32G32B32_UINT";
        case VK_FORMAT_R32G32B32_SINT: return "R32G32B32_SINT";
        case VK_FORMAT_R32G32B32_SFLOAT: return "R32G32B32_SFLOAT";
        case VK_FORMAT_R32G32B32A32_UINT: return "R32G32B32A32_UINT";
        case VK_FORMAT_R32G32B32A32_SINT: return "R32G32B32A32_SINT";
        case VK_FORMAT_R32G32B32A32_SFLOAT: return "R32G32B32A32_SFLOAT";
        case VK_FORMAT_R64_UINT: return "R64_UINT";
        case VK_FORMAT_R64_SINT: return "R64_SINT";
        case VK_FORMAT_R64_SFLOAT: return "R64_SFLOAT";
        case VK_FORMAT_R64G64_UINT: return "R64G64_UINT";
        case VK_FORMAT_R64G64_SINT: return "R64G64_SINT";
        case VK_FORMAT_R64G64_SFLOAT: return "R64G64_SFLOAT";
        case VK_FORMAT_R64G64B64_UINT: return "R64G64B64_UINT";
        case VK_FORMAT_R64G64B64_SINT: return "R64G64B64_SINT";
        case VK_FORMAT_R64G64B64_SFLOAT: return "R64G64B64_SFLOAT";
        case VK_FORMAT_R64G64B64A64_UINT: return "R64G64B64A64_UINT";
        case VK_FORMAT_R64G64B64A64_SINT: return "R64G64B64A64_SINT";
        case VK_FORMAT_R64G64B64A64_SFLOAT: return "R64G64B64A64_SFLOAT";
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return "B10G11R11_UFLOAT_PACK32";
        case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: return "E5B9G9R9_UFLOAT_PACK32";
        case VK_FORMAT_D16_UNORM: return "D16_UNORM";
        case VK_FORMAT_X8_D24_UNORM_PACK32: return "X8_D24_UNORM_PACK32";
        case VK_FORMAT_D32_SFLOAT: return "D32_SFLOAT";
        case VK_FORMAT_S8_UINT: return "S8_UINT";
        case VK_FORMAT_D16_UNORM_S8_UINT: return "D16_UNORM_S8_UINT";
        case VK_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
        case VK_FORMAT_D32_SFLOAT_S8_UINT: return "D32_SFLOAT_S8_UINT";
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK: return "BC1_RGB_UNORM_BLOCK";
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK: return "BC1_RGB_SRGB_BLOCK";
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return "BC1_RGBA_UNORM_BLOCK";
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return "BC1_RGBA_SRGB_BLOCK";
        case VK_FORMAT_BC2_UNORM_BLOCK: return "BC2_UNORM_BLOCK";
        case VK_FORMAT_BC2_SRGB_BLOCK: return "BC2_SRGB_BLOCK";
        case VK_FORMAT_BC3_UNORM_BLOCK: return "BC3_UNORM_BLOCK";
        case VK_FORMAT_BC3_SRGB_BLOCK: return "BC3_SRGB_BLOCK";
        case VK_FORMAT_BC4_UNORM_BLOCK: return "BC4_UNORM_BLOCK";
        case VK_FORMAT_BC4_SNORM_BLOCK: return "BC4_SNORM_BLOCK";
        case VK_FORMAT_BC5_UNORM_BLOCK: return "BC5_UNORM_BLOCK";
        case VK_FORMAT_BC5_SNORM_BLOCK: return "BC5_SNORM_BLOCK";
        case VK_FORMAT_BC6H_UFLOAT_BLOCK: return "BC6H_UFLOAT_BLOCK";
        case VK_FORMAT_BC6H_SFLOAT_BLOCK: return "BC6H_SFLOAT_BLOCK";
        case VK_FORMAT_BC7_UNORM_BLOCK: return "BC7_UNORM_BLOCK";
        case VK_FORMAT_BC7_SRGB_BLOCK: return "BC7_SRGB_BLOCK";
        case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK: return "ETC2_R8G8B8_UNORM_BLOCK";
        case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK: return "ETC2_R8G8B8_SRGB_BLOCK";
        case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK: return "ETC2_R8G8B8A1_UNORM_BLOCK";
        case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK: return "ETC2_R8G8B8A1_SRGB_BLOCK";
        case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK: return "ETC2_R8G8B8A8_UNORM_BLOCK";
        case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK: return "ETC2_R8G8B8A8_SRGB_BLOCK";
        case VK_FORMAT_EAC_R11_UNORM_BLOCK: return "EAC_R11_UNORM_BLOCK";
        case VK_FORMAT_EAC_R11_SNORM_BLOCK: return "EAC_R11_SNORM_BLOCK";
        case VK_FORMAT_EAC_R11G11_UNORM_BLOCK: return "EAC_R11G11_UNORM_BLOCK";
        case VK_FORMAT_EAC_R11G11_SNORM_BLOCK: return "EAC_R11G11_SNORM_BLOCK";
        case VK_FORMAT_ASTC_4x4_UNORM_BLOCK: return "ASTC_4x4_UNORM_BLOCK";
        case VK_FORMAT_ASTC_4x4_SRGB_BLOCK: return "ASTC_4x4_SRGB_BLOCK";
        case VK_FORMAT_ASTC_5x4_UNORM_BLOCK: return "ASTC_5x4_UNORM_BLOCK";
        case VK_FORMAT_ASTC_5x4_SRGB_BLOCK: return "ASTC_5x4_SRGB_BLOCK";
        case VK_FORMAT_ASTC_5x5_UNORM_BLOCK: return "ASTC_5x5_UNORM_BLOCK";
        case VK_FORMAT_ASTC_5x5_SRGB_BLOCK: return "ASTC_5x5_SRGB_BLOCK";
        case VK_FORMAT_ASTC_6x5_UNORM_BLOCK: return "ASTC_6x5_UNORM_BLOCK";
        case VK_FORMAT_ASTC_6x5_SRGB_BLOCK: return "ASTC_6x5_SRGB_BLOCK";
        case VK_FORMAT_ASTC_6x6_UNORM_BLOCK: return "ASTC_6x6_UNORM_BLOCK";
        case VK_FORMAT_ASTC_6x6_SRGB_BLOCK: return "ASTC_6x6_SRGB_BLOCK";
        case VK_FORMAT_ASTC_8x5_UNORM_BLOCK: return "ASTC_8x5_UNORM_BLOCK";
        case VK_FORMAT_ASTC_8x5_SRGB_BLOCK: return "ASTC_8x5_SRGB_BLOCK";
        case VK_FORMAT_ASTC_8x6_UNORM_BLOCK: return "ASTC_8x6_UNORM_BLOCK";
        case VK_FORMAT_ASTC_8x6_SRGB_BLOCK: return "ASTC_8x6_SRGB_BLOCK";
        case VK_FORMAT_ASTC_8x8_UNORM_BLOCK: return "ASTC_8x8_UNORM_BLOCK";
        case VK_FORMAT_ASTC_8x8_SRGB_BLOCK: return "ASTC_8x8_SRGB_BLOCK";
        case VK_FORMAT_ASTC_10x5_UNORM_BLOCK: return "ASTC_10x5_UNORM_BLOCK";
        case VK_FORMAT_ASTC_10x5_SRGB_BLOCK: return "ASTC_10x5_SRGB_BLOCK";
        case VK_FORMAT_ASTC_10x6_UNORM_BLOCK: return "ASTC_10x6_UNORM_BLOCK";
        case VK_FORMAT_ASTC_10x6_SRGB_BLOCK: return "ASTC_10x6_SRGB_BLOCK";
        case VK_FORMAT_ASTC_10x8_UNORM_BLOCK: return "ASTC_10x8_UNORM_BLOCK";
        case VK_FORMAT_ASTC_10x8_SRGB_BLOCK: return "ASTC_10x8_SRGB_BLOCK";
        case VK_FORMAT_ASTC_10x10_UNORM_BLOCK: return "ASTC_10x10_UNORM_BLOCK";
        case VK_FORMAT_ASTC_10x10_SRGB_BLOCK: return "ASTC_10x10_SRGB_BLOCK";
        case VK_FORMAT_ASTC_12x10_UNORM_BLOCK: return "ASTC_12x10_UNORM_BLOCK";
        case VK_FORMAT_ASTC_12x10_SRGB_BLOCK: return "ASTC_12x10_SRGB_BLOCK";
        case VK_FORMAT_ASTC_12x12_UNORM_BLOCK: return "ASTC_12x12_UNORM_BLOCK";
        case VK_FORMAT_ASTC_12x12_SRGB_BLOCK: return "ASTC_12x12_SRGB_BLOCK";
        case VK_FORMAT_G8B8G8R8_422_UNORM: return "G8B8G8R8_422_UNORM";
        case VK_FORMAT_B8G8R8G8_422_UNORM: return "B8G8R8G8_422_UNORM";
        case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM: return "G8_B8_R8_3PLANE_420_UNORM";
        case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM: return "G8_B8R8_2PLANE_420_UNORM";
        case VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM: return "G8_B8_R8_3PLANE_422_UNORM";
        case VK_FORMAT_G8_B8R8_2PLANE_422_UNORM: return "G8_B8R8_2PLANE_422_UNORM";
        case VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM: return "G8_B8_R8_3PLANE_444_UNORM";
        case VK_FORMAT_R10X6_UNORM_PACK16: return "R10X6_UNORM_PACK16";
        case VK_FORMAT_R10X6G10X6_UNORM_2PACK16: return "R10X6G10X6_UNORM_2PACK16";
        case VK_FORMAT_R10X6G10X6B10X6A10X6_UNORM_4PACK16: return "R10X6G10X6B10X6A10X6_UNORM_4PACK16";
        case VK_FORMAT_G10X6B10X6G10X6R10X6_422_UNORM_4PACK16: return "G10X6B10X6G10X6R10X6_422_UNORM_4PACK16";
        case VK_FORMAT_B10X6G10X6R10X6G10X6_422_UNORM_4PACK16: return "B10X6G10X6R10X6G10X6_422_UNORM_4PACK16";
        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16: return "G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16";
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16: return "G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16";
        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16: return "G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16";
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16: return "G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16";
        case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16: return "G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16";
        case VK_FORMAT_R12X4_UNORM_PACK16: return "R12X4_UNORM_PACK16";
        case VK_FORMAT_R12X4G12X4_UNORM_2PACK16: return "R12X4G12X4_UNORM_2PACK16";
        case VK_FORMAT_R12X4G12X4B12X4A12X4_UNORM_4PACK16: return "R12X4G12X4B12X4A12X4_UNORM_4PACK16";
        case VK_FORMAT_G12X4B12X4G12X4R12X4_422_UNORM_4PACK16: return "G12X4B12X4G12X4R12X4_422_UNORM_4PACK16";
        case VK_FORMAT_B12X4G12X4R12X4G12X4_422_UNORM_4PACK16: return "B12X4G12X4R12X4G12X4_422_UNORM_4PACK16";
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16: return "G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16";
        case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16: return "G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16";
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16: return "G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16";
        case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16: return "G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16";
        case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16: return "G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16";
        case VK_FORMAT_G16B16G16R16_422_UNORM: return "G16B16G16R16_422_UNORM";
        case VK_FORMAT_B16G16R16G16_422_UNORM: return "B16G16R16G16_422_UNORM";
        case VK_FORMAT_G16_B16_R16_3PLANE_420_UNORM: return "G16_B16_R16_3PLANE_420_UNORM";
        case VK_FORMAT_G16_B16R16_2PLANE_420_UNORM: return "G16_B16R16_2PLANE_420_UNORM";
        case VK_FORMAT_G16_B16_R16_3PLANE_422_UNORM: return "G16_B16_R16_3PLANE_422_UNORM";
        case VK_FORMAT_G16_B16R16_2PLANE_422_UNORM: return "G16_B16R16_2PLANE_422_UNORM";
        case VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM: return "G16_B16_R16_3PLANE_444_UNORM";
        case VK_FORMAT_G8_B8R8_2PLANE_444_UNORM: return "G8_B8R8_2PLANE_444_UNORM";
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16: return "G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16";
        case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_444_UNORM_3PACK16: return "G12X4_B12X4R12X4_2PLANE_444_UNORM_3PACK16";
        case VK_FORMAT_G16_B16R16_2PLANE_444_UNORM: return "G16_B16R16_2PLANE_444_UNORM";
        case VK_FORMAT_A4R4G4B4_UNORM_PACK16: return "A4R4G4B4_UNORM_PACK16";
        case VK_FORMAT_A4B4G4R4_UNORM_PACK16: return "A4B4G4R4_UNORM_PACK16";
        case VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK: return "ASTC_4x4_SFLOAT_BLOCK";
        case VK_FORMAT_ASTC_5x4_SFLOAT_BLOCK: return "ASTC_5x4_SFLOAT_BLOCK";
        case VK_FORMAT_ASTC_5x5_SFLOAT_BLOCK: return "ASTC_5x5_SFLOAT_BLOCK";
        case VK_FORMAT_ASTC_6x5_SFLOAT_BLOCK: return "ASTC_6x5_SFLOAT_BLOCK";
        case VK_FORMAT_ASTC_6x6_SFLOAT_BLOCK: return "ASTC_6x6_SFLOAT_BLOCK";
        case VK_FORMAT_ASTC_8x5_SFLOAT_BLOCK: return "ASTC_8x5_SFLOAT_BLOCK";
        case VK_FORMAT_ASTC_8x6_SFLOAT_BLOCK: return "ASTC_8x6_SFLOAT_BLOCK";
        case VK_FORMAT_ASTC_8x8_SFLOAT_BLOCK: return "ASTC_8x8_SFLOAT_BLOCK";
        case VK_FORMAT_ASTC_10x5_SFLOAT_BLOCK: return "ASTC_10x5_SFLOAT_BLOCK";
        case VK_FORMAT_ASTC_10x6_SFLOAT_BLOCK: return "ASTC_10x6_SFLOAT_BLOCK";
        case VK_FORMAT_ASTC_10x8_SFLOAT_BLOCK: return "ASTC_10x8_SFLOAT_BLOCK";
        case VK_FORMAT_ASTC_10x10_SFLOAT_BLOCK: return "ASTC_10x10_SFLOAT_BLOCK";
        case VK_FORMAT_ASTC_12x10_SFLOAT_BLOCK: return "ASTC_12x10_SFLOAT_BLOCK";
        case VK_FORMAT_ASTC_12x12_SFLOAT_BLOCK: return "ASTC_12x12_SFLOAT_BLOCK";
        case VK_FORMAT_A1B5G5R5_UNORM_PACK16: return "A1B5G5R5_UNORM_PACK16";
        case VK_FORMAT_A8_UNORM: return "A8_UNORM";
        case VK_FORMAT_PVRTC1_2BPP_UNORM_BLOCK_IMG: return "PVRTC1_2BPP_UNORM_BLOCK_IMG";
        case VK_FORMAT_PVRTC1_4BPP_UNORM_BLOCK_IMG: return "PVRTC1_4BPP_UNORM_BLOCK_IMG";
        case VK_FORMAT_PVRTC2_2BPP_UNORM_BLOCK_IMG: return "PVRTC2_2BPP_UNORM_BLOCK_IMG";
        case VK_FORMAT_PVRTC2_4BPP_UNORM_BLOCK_IMG: return "PVRTC2_4BPP_UNORM_BLOCK_IMG";
        case VK_FORMAT_PVRTC1_2BPP_SRGB_BLOCK_IMG: return "PVRTC1_2BPP_SRGB_BLOCK_IMG";
        case VK_FORMAT_PVRTC1_4BPP_SRGB_BLOCK_IMG: return "PVRTC1_4BPP_SRGB_BLOCK_IMG";
        case VK_FORMAT_PVRTC2_2BPP_SRGB_BLOCK_IMG: return "PVRTC2_2BPP_SRGB_BLOCK_IMG";
        case VK_FORMAT_PVRTC2_4BPP_SRGB_BLOCK_IMG: return "PVRTC2_4BPP_SRGB_BLOCK_IMG";
        case VK_FORMAT_R16G16_SFIXED5_NV: return "R16G16_SFIXED5_NV";
        default: return "UNKNOWN";
    }
}

std::string_view to_string(VkPresentModeKHR v) noexcept {
    switch (v) {
        case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE_KHR";
        case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX_KHR";
        case VK_PRESENT_MODE_FIFO_KHR: return "FIFO_KHR";
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED_KHR";
        case VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR: return "SHARED_DEMAND_REFRESH_KHR";
        case VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR: return "SHARED_CONTINUOUS_REFRESH_KHR";
        default: return "UNKNOWN";
    }
}

}  // namespace radray::render::vulkan
