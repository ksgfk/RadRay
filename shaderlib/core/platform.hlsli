#ifndef RADRAY_CORE_PLATFORM_HLSLI
#define RADRAY_CORE_PLATFORM_HLSLI

#if defined(__spirv__)
#define VK_LOCATION(location) [[vk::location(location)]]
#define VK_BINDING(register_index, set_index) [[vk::binding(register_index, set_index)]]
#define VK_PUSH_CONSTANT [[vk::push_constant]]
#else
#define VK_LOCATION(location)
#define VK_BINDING(register_index, set_index)
#define VK_PUSH_CONSTANT
#endif

#endif
