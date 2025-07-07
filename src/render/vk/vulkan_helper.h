#pragma once

#include <span>
#include <algorithm>

#include <radray/platform.h>
#include <radray/types.h>
#include <radray/logger.h>
#include <radray/utility.h>
#include <radray/render/common.h>

#include "vulkan_common.h"
#include "volk.h"
#include "vk_mem_alloc.h"

namespace radray::render::vulkan {

class DeviceVulkan;
class FenceVulkan;

using FTbVk = VolkDeviceTable;

template <typename T>
class VkObjectDeleter {
    static_assert(false, "unsupported Vulkan object type");
};
template <>
class VkObjectDeleter<VkSurfaceKHR> {
public:
    void operator()(VkSurfaceKHR surface) const noexcept;

public:
    VkInstance _instance{VK_NULL_HANDLE};
    const VkAllocationCallbacks* _allocCb{nullptr};
};
template <>
class VkObjectDeleter<VkSwapchainKHR> {
public:
    void operator()(VkSwapchainKHR swapchain) const noexcept;

public:
    PFN_vkDestroySwapchainKHR f{nullptr};
    VkDevice _device{VK_NULL_HANDLE};
    const VkAllocationCallbacks* _allocCb{nullptr};
};

template <typename T, typename TDeleter = VkObjectDeleter<T>>
requires std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T> && std::is_default_constructible_v<TDeleter>
class VkObjectWrapper {
public:
    constexpr VkObjectWrapper() noexcept = default;

    constexpr VkObjectWrapper(T obj, TDeleter&& deleter) noexcept
        : _obj(std::move(obj)),
          _deleter(std::move(deleter)) {}

    constexpr VkObjectWrapper(T obj, const TDeleter& deleter) noexcept
        : _obj(std::move(obj)),
          _deleter(deleter) {}

    constexpr VkObjectWrapper(const VkObjectWrapper&) = delete;
    constexpr VkObjectWrapper& operator=(const VkObjectWrapper&) = delete;

    constexpr VkObjectWrapper(VkObjectWrapper&& other) noexcept
        : _obj(std::move(other._obj)),
          _deleter(std::move(other._deleter)) {
        other._obj = VK_NULL_HANDLE;
        other._deleter = TDeleter{};
    }

    constexpr VkObjectWrapper& operator=(VkObjectWrapper&& other) noexcept {
        if (this != &other) {
            VkObjectWrapper temp{std::move(other)};
            swap(*this, temp);
        }
        return *this;
    }

    constexpr ~VkObjectWrapper() noexcept {
        this->Destroy();
    }

    friend constexpr void swap(VkObjectWrapper& l, VkObjectWrapper& r) noexcept {
        using std::swap;
        swap(l._obj, r._obj);
        swap(l._deleter, r._deleter);
    }

    constexpr T Get() const noexcept {
        return _obj;
    }

    constexpr auto Release() noexcept {
        T temp = _obj;
        _obj = VK_NULL_HANDLE;
        return temp;
    }

    constexpr bool IsValid() const noexcept {
        return _obj != VK_NULL_HANDLE;
    }

    constexpr void Destroy() noexcept {
        if (this->IsValid()) {
            _deleter.operator()(_obj);
            _obj = VK_NULL_HANDLE;
        }
    }

public:
    T _obj{VK_NULL_HANDLE};
    TDeleter _deleter{};
};

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
bool IsValidateExtensions(std::span<const char*> required, std::span<VkExtensionProperties> available) noexcept;
bool IsValidateLayers(std::span<const char*> required, std::span<VkLayerProperties> available) noexcept;
std::optional<VkSurfaceFormatKHR> SelectSurfaceFormat(VkPhysicalDevice gpu, VkSurfaceKHR surface, std::span<VkFormat> preferred) noexcept;

VkQueueFlags MapType(QueueType v) noexcept;
VkFormat MapType(TextureFormat v) noexcept;
VkImageType MapType(TextureDimension v) noexcept;

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
