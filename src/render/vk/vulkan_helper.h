#pragma once

#include <span>
#include <algorithm>

#include <radray/platform.h>
#include <radray/types.h>
#include <radray/logger.h>
#include <radray/utility.h>
#include <radray/basic_math.h>
#include <radray/render/common.h>

#include "vulkan_common.h"
#include "volk.h"
#include "vk_mem_alloc.h"

namespace radray::render::vulkan {

using DeviceFuncTable = VolkDeviceTable;

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

uint64_t GetPhysicalDeviceMemoryAllSize(const VkPhysicalDeviceMemoryProperties& memory, VkMemoryHeapFlags heapFlags) noexcept;
bool IsValidateExtensions(std::span<const char*> required, std::span<VkExtensionProperties> available) noexcept;
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
