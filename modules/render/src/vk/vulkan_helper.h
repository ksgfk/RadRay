#pragma once

#include <span>
#include <algorithm>

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
#else
#error "unknown platform"
#endif
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <volk.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored  "-Wnullability-completeness"
#pragma clang diagnostic ignored  "-Wunused-parameter"
#pragma clang diagnostic ignored  "-Wmissing-field-initializers"
#include <vk_mem_alloc.h>
#pragma clang diagnostic pop

namespace radray::render::vulkan {

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
VkImageViewType MapType(TextureViewDimension v) noexcept;
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

std::string_view FormatVkDebugUtilsMessageTypeFlagsEXT(VkDebugUtilsMessageTypeFlagsEXT v) noexcept;
std::string_view FormatVkQueueFlags(VkQueueFlags v) noexcept;
std::string_view to_string(VkPhysicalDeviceType v) noexcept;
std::string_view to_string(VkResult v) noexcept;
std::string_view to_string(VkFormat v) noexcept;
std::string_view to_string(VkPresentModeKHR v) noexcept;

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
