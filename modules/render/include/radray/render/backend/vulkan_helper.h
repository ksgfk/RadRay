#pragma once

#ifdef RADRAY_ENABLE_VULKAN

#include <span>

#include <radray/platform.h>
#include <radray/types.h>
#include <radray/logger.h>
#include <radray/utility.h>
#include <radray/basic_math.h>
#include <radray/render/common.h>

#ifdef RADRAY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WINDOWS
#define _WINDOWS
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#define VK_USE_PLATFORM_WIN32_KHR
#elif RADRAY_PLATFORM_MACOS
#define VK_USE_PLATFORM_METAL_EXT
#else
#error "unknown platform"
#endif
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <volk.h>
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif
#ifdef _MSC_VER
#pragma warning(push, 1)
#endif
#include <vk_mem_alloc.h>
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace radray::render::vulkan {

template <class T>
struct VulkanObjectTrait {
    static_assert(false, "unknown vulkan object type");
};

template <class T>
struct VulkanObjectTrait<T const> : VulkanObjectTrait<T> {};
template <class T>
struct VulkanObjectTrait<T volatile> : VulkanObjectTrait<T> {};
template <class T>
struct VulkanObjectTrait<T const volatile> : VulkanObjectTrait<T> {};

template <>
struct VulkanObjectTrait<VkInstance> {
    static constexpr VkObjectType type = VK_OBJECT_TYPE_INSTANCE;
};
template <>
struct VulkanObjectTrait<VkPhysicalDevice> {
    static constexpr VkObjectType type = VK_OBJECT_TYPE_PHYSICAL_DEVICE;
};
template <>
struct VulkanObjectTrait<VkDevice> {
    static constexpr VkObjectType type = VK_OBJECT_TYPE_DEVICE;
};
template <>
struct VulkanObjectTrait<VkQueue> {
    static constexpr VkObjectType type = VK_OBJECT_TYPE_QUEUE;
};
template <>
struct VulkanObjectTrait<VkSemaphore> {
    static constexpr VkObjectType type = VK_OBJECT_TYPE_SEMAPHORE;
};
template <>
struct VulkanObjectTrait<VkCommandBuffer> {
    static constexpr VkObjectType type = VK_OBJECT_TYPE_COMMAND_BUFFER;
};
template <>
struct VulkanObjectTrait<VkFence> {
    static constexpr VkObjectType type = VK_OBJECT_TYPE_FENCE;
};
template <>
struct VulkanObjectTrait<VkDeviceMemory> {
    static constexpr VkObjectType type = VK_OBJECT_TYPE_DEVICE_MEMORY;
};
template <>
struct VulkanObjectTrait<VkBuffer> {
    static constexpr VkObjectType type = VK_OBJECT_TYPE_BUFFER;
};
template <>
struct VulkanObjectTrait<VkImage> {
    static constexpr VkObjectType type = VK_OBJECT_TYPE_IMAGE;
};
template <>
struct VulkanObjectTrait<VkEvent> {
    static constexpr VkObjectType type = VK_OBJECT_TYPE_EVENT;
};
template <>
struct VulkanObjectTrait<VkQueryPool> {
    static constexpr VkObjectType type = VK_OBJECT_TYPE_QUERY_POOL;
};
template <>
struct VulkanObjectTrait<VkBufferView> {
    static constexpr VkObjectType type = VK_OBJECT_TYPE_BUFFER_VIEW;
};
template <>
struct VulkanObjectTrait<VkImageView> {
    static constexpr VkObjectType type = VK_OBJECT_TYPE_IMAGE_VIEW;
};
template <>
struct VulkanObjectTrait<VkShaderModule> {
    static constexpr VkObjectType type = VK_OBJECT_TYPE_SHADER_MODULE;
};
template <>
struct VulkanObjectTrait<VkPipelineCache> {
    static constexpr VkObjectType type = VK_OBJECT_TYPE_PIPELINE_CACHE;
};
template <>
struct VulkanObjectTrait<VkPipelineLayout> {
    static constexpr VkObjectType type = VK_OBJECT_TYPE_PIPELINE_LAYOUT;
};
template <>
struct VulkanObjectTrait<VkRenderPass> {
    static constexpr VkObjectType type = VK_OBJECT_TYPE_RENDER_PASS;
};
template <>
struct VulkanObjectTrait<VkPipeline> {
    static constexpr VkObjectType type = VK_OBJECT_TYPE_PIPELINE;
};
template <>
struct VulkanObjectTrait<VkDescriptorSetLayout> {
    static constexpr VkObjectType type = VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT;
};
template <>
struct VulkanObjectTrait<VkSampler> {
    static constexpr VkObjectType type = VK_OBJECT_TYPE_SAMPLER;
};
template <>
struct VulkanObjectTrait<VkDescriptorPool> {
    static constexpr VkObjectType type = VK_OBJECT_TYPE_DESCRIPTOR_POOL;
};
template <>
struct VulkanObjectTrait<VkDescriptorSet> {
    static constexpr VkObjectType type = VK_OBJECT_TYPE_DESCRIPTOR_SET;
};
template <>
struct VulkanObjectTrait<VkFramebuffer> {
    static constexpr VkObjectType type = VK_OBJECT_TYPE_FRAMEBUFFER;
};
template <>
struct VulkanObjectTrait<VkCommandPool> {
    static constexpr VkObjectType type = VK_OBJECT_TYPE_COMMAND_POOL;
};

template <typename T, typename TAllocator, typename TFunc, typename... Args>
requires std::invocable<TFunc, Args..., uint32_t*, T*> && std::is_same_v<std::invoke_result_t<TFunc, Args..., uint32_t*, T*>, void>
auto EnumerateVectorFromVkFunc(std::vector<T, TAllocator>& out, TFunc&& f, Args&&... args) noexcept {
    uint32_t count = 0;
    f(std::forward<Args>(args)..., &count, nullptr);
    if (count == 0) {
        return;
    }
    out.resize(count);
    f(std::forward<Args>(args)..., &count, out.data());
    return;
}

template <typename T, typename TAllocator, typename TFunc, typename... Args>
requires std::invocable<TFunc, Args..., uint32_t*, T*> && std::is_same_v<std::invoke_result_t<TFunc, Args..., uint32_t*, T*>, VkResult>
auto EnumerateVectorFromVkFunc(std::vector<T, TAllocator>& out, TFunc&& f, Args&&... args) noexcept -> VkResult {
    uint32_t count = 0;
    VkResult err;
    do {
        err = f(std::forward<Args>(args)..., &count, nullptr);
        if (err != VK_SUCCESS) {
            return err;
        }
        out.resize(count);
        err = f(std::forward<Args>(args)..., &count, out.data());
    } while (err == VK_INCOMPLETE);
    return err;
}

template <class T, class N>
void AddToHeadVulkanStruct(T& target, N& add) noexcept {
    auto t = target.pNext;
    target.pNext = &add;
    add.pNext = t;
}

uint64_t GetPhysicalDeviceMemoryAllSize(const VkPhysicalDeviceMemoryProperties& memory, VkMemoryHeapFlags heapFlags) noexcept;
bool IsValidateExtensions(std::span<const char*> required, std::span<VkExtensionProperties> available) noexcept;
bool IsValidateExtensions(std::string_view required, std::span<VkExtensionProperties> available) noexcept;
bool IsValidateLayers(std::span<const char*> required, std::span<VkLayerProperties> available) noexcept;
std::optional<VkSurfaceFormatKHR> SelectSurfaceFormat(VkPhysicalDevice gpu, VkSurfaceKHR surface, std::span<VkFormat> preferred) noexcept;

VkImageAspectFlags ImageFormatToAspectFlags(VkFormat v) noexcept;

VkAccessFlags BufferUseToAccessFlags(BufferUses v) noexcept;
VkPipelineStageFlags BufferUseToPipelineStageFlags(BufferUses v) noexcept;

VkAccessFlags TextureUseToAccessFlags(TextureUses v) noexcept;
VkPipelineStageFlags TextureUseToPipelineStageFlags(TextureUses v, bool isSrc) noexcept;
VkImageLayout TextureUseToLayout(TextureUses v) noexcept;

VkQueueFlags MapType(QueueType v) noexcept;
VkFormat MapType(TextureFormat v) noexcept;
VkImageType MapType(TextureDimension v) noexcept;
VkSampleCountFlagBits MapSampleCount(uint32_t v) noexcept;
VkImageViewType MapViewType(TextureDimension v) noexcept;
VkAttachmentLoadOp MapType(LoadAction v) noexcept;
VkAttachmentStoreOp MapType(StoreAction v) noexcept;
VmaMemoryUsage MapType(MemoryType v) noexcept;
VkShaderStageFlags MapType(ShaderStages v) noexcept;
VkDescriptorType MapType(ResourceBindType v) noexcept;
VkVertexInputRate MapType(VertexStepMode v) noexcept;
VkFormat MapType(VertexFormat v) noexcept;
VkPrimitiveTopology MapType(PrimitiveTopology v) noexcept;
VkPolygonMode MapType(PolygonMode v) noexcept;
VkCullModeFlags MapType(CullMode v) noexcept;
VkFrontFace MapType(FrontFace v) noexcept;
VkCompareOp MapType(CompareFunction v) noexcept;
VkStencilOpState MapType(StencilFaceState v, uint32_t readMask, uint32_t writeMask) noexcept;
VkStencilOp MapType(StencilOperation v) noexcept;
struct BlendComponentVulkan {
    VkBlendOp op;
    VkBlendFactor src;
    VkBlendFactor dst;
};
BlendComponentVulkan MapType(BlendComponent v) noexcept;
VkBlendOp MapType(BlendOperation v) noexcept;
VkBlendFactor MapType(BlendFactor v) noexcept;
VkColorComponentFlags MapType(ColorWrites v) noexcept;
VkIndexType MapIndexType(uint32_t v) noexcept;
VkFilter MapTypeFilter(FilterMode v) noexcept;
VkSamplerMipmapMode MapTypeMipmapMode(FilterMode v) noexcept;
VkSamplerAddressMode MapType(AddressMode v) noexcept;
VkPresentModeKHR MapType(PresentMode v) noexcept;

std::string_view FormatVkDebugUtilsMessageTypeFlagsEXT(VkDebugUtilsMessageTypeFlagsEXT v) noexcept;
std::string_view FormatVkQueueFlags(VkQueueFlags v) noexcept;
std::string_view to_string(VkPhysicalDeviceType v) noexcept;
std::string_view to_string(VkResult v) noexcept;
std::string_view to_string(VkFormat v) noexcept;
std::string_view to_string(VkPresentModeKHR v) noexcept;
std::string_view to_string(VkDescriptorType v) noexcept;

}  // namespace radray::render::vulkan

template <class VkType, class CharT>
struct RadrayVkTypeFormat : fmt::formatter<std::string_view, CharT> {
    template <class FormatContext>
    auto format(VkType val, FormatContext& ctx) const {
        return fmt::formatter<std::string_view, CharT>::format(radray::render::vulkan::to_string(val), ctx);
    }
};

template <class CharT>
struct fmt::formatter<VkPhysicalDeviceType, CharT> : RadrayVkTypeFormat<VkPhysicalDeviceType, CharT> {};
template <class CharT>
struct fmt::formatter<VkResult, CharT> : RadrayVkTypeFormat<VkResult, CharT> {};
template <class CharT>
struct fmt::formatter<VkFormat, CharT> : RadrayVkTypeFormat<VkFormat, CharT> {};
template <class CharT>
struct fmt::formatter<VkPresentModeKHR, CharT> : RadrayVkTypeFormat<VkPresentModeKHR, CharT> {};
template <class CharT>
struct fmt::formatter<VkDescriptorType, CharT> : RadrayVkTypeFormat<VkDescriptorType, CharT> {};

#endif
