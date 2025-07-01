#pragma once

#include <radray/platform.h>
#include <radray/types.h>
#include <radray/logger.h>
#include <radray/utility.h>
#include <radray/render/common.h>

#ifdef RADRAY_PLATFORM_WINDOWS
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <volk.h>
#include <vk_mem_alloc.h>

namespace radray::render::vulkan {

class DeviceVulkan;
class FenceVulkan;

using FTbVk = VolkDeviceTable;

template <typename T, typename F, typename... Args>
requires std::invocable<F, Args..., uint32_t*, T*>
auto GetVector(vector<T>& out, F&& f, Args&&... ts) noexcept -> VkResult {
    uint32_t count = 0;
    if constexpr (std::is_same_v<std::invoke_result_t<F, Args..., uint32_t*, T*>, void>) {
        f(std::forward<Args>(ts)..., &count, nullptr);
        if (count == 0) {
            return VK_SUCCESS;
        }
        out.resize(count);
        f(std::forward<Args>(ts)..., &count, out.data());
        return VK_SUCCESS;
    } else if constexpr (std::is_same_v<std::invoke_result_t<F, Args..., uint32_t*, T*>, VkResult>) {
        VkResult err;
        do {
            err = f(std::forward<Args>(ts)..., &count, nullptr);
            if (err != VK_SUCCESS) {
                return err;
            }
            out.resize(count);
            err = f(std::forward<Args>(ts)..., &count, out.data());
        } while (err == VK_INCOMPLETE);
        return err;
    } else {
        static_assert(false, "GetVector: F must return void or VkResult");
    }
}

uint64_t GetPhysicalDeviceMemoryAllSize(const VkPhysicalDeviceMemoryProperties& memory, VkMemoryHeapFlags heapFlags) noexcept;

VkQueueFlags MapType(QueueType v) noexcept;
VkFormat MapType(TextureFormat v) noexcept;
VkImageType MapType(TextureDimension v) noexcept;

std::string_view FormatVkDebugUtilsMessageTypeFlagsEXT(VkDebugUtilsMessageTypeFlagsEXT v) noexcept;
std::string_view FormatVkQueueFlags(VkQueueFlags v) noexcept;
std::string_view to_string(VkPhysicalDeviceType v) noexcept;
std::string_view to_string(VkResult v) noexcept;

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
