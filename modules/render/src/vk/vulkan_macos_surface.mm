#include <radray/render/backend/vulkan_helper.h>

#ifdef RADRAY_ENABLE_VULKAN

#import <QuartzCore/CAMetalLayer.h>

namespace radray {

VkSurfaceKHR CreateMacOSMetalSurface(VkInstance instance, void* nativeHandler, const VkAllocationCallbacks* allocator) noexcept {
    CAMetalLayer* metalLayer = (__bridge CAMetalLayer*)nativeHandler;
    if (!metalLayer) {
        return VK_NULL_HANDLE;
    }
    VkMetalSurfaceCreateInfoEXT createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
    createInfo.pNext = nullptr;
    createInfo.flags = 0;
    createInfo.pLayer = metalLayer;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkResult vr = vkCreateMetalSurfaceEXT(instance, &createInfo, allocator, &surface);
    if (vr != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return surface;
}

}  // namespace radray

#endif
