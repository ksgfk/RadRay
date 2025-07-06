#pragma once

#ifdef RADRAY_PLATFORM_WINDOWS
#define VK_USE_PLATFORM_WIN32_KHR
#else
#error "unknown platform"
#endif
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#ifdef __cplusplus
namespace radray::render::vulkan {
}
#endif
