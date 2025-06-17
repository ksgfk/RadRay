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

namespace radray::render::vulkan {

template <typename T>
concept is_vk_struct = requires(T t) {
    { t.sType } -> std::convertible_to<VkStructureType>;
    { t.pNext } -> std::convertible_to<const void*>;
};

template <typename TBase, typename TNext>
requires is_vk_struct<TBase> && is_vk_struct<TNext>
constexpr void SetVkStructPtrToLast(TBase* v, TNext* pNext) noexcept {
    if (v->pNext == nullptr) {
        v->pNext = pNext;
    } else {
        VkBaseOutStructure* current = reinterpret_cast<VkBaseOutStructure*>(v);
        while (current->pNext != nullptr) {
            current = current->pNext;
        }
        current->pNext = reinterpret_cast<VkBaseOutStructure*>(pNext);
    }
}

std::string_view formatVkDebugMsgType(VkDebugUtilsMessageTypeFlagsEXT v) noexcept;

}  // namespace radray::render::vulkan
