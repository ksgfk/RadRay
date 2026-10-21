#ifndef RADRAY_CORE_PLATFORM_HLSLI
#define RADRAY_CORE_PLATFORM_HLSLI

#if defined(__spirv__)
#define VK_LOCATION(index) [[vk::location(index)]]
#define VK_BINDING(register_index, set_index) [[vk::binding(register_index, set_index)]]
#define VK_PUSH_CONSTANT [[vk::push_constant]]
#define VK_IMAGE_FORMAT(format) [[vk::image_format(format)]]
#else
#define VK_LOCATION(location)
#define VK_BINDING(register_index, set_index)
#define VK_PUSH_CONSTANT
#define VK_IMAGE_FORMAT(format)
#endif

#endif
