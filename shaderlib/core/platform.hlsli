#ifndef RADRAY_CORE_PLATFORM_HLSLI
#define RADRAY_CORE_PLATFORM_HLSLI

// 跨后端差异的唯一收口处。这里只放"同一份声明如何在不同后端展开"的 shim,
// 不放任何数学或着色逻辑 —— 换后端时只需要读这一个文件。
//
// DXC 编译到 SPIR-V / Metal 时需要显式的 location / binding / push_constant 标注,
// 编译到 DXIL 时这些标注非法。成套提供后, shader 只写一份绑定声明即可两边通吃。

#if defined(VULKAN) || defined(METAL)
#define VK_LOCATION(l) [[vk::location(l)]]
#define VK_BINDING(b, s) [[vk::binding(b, s)]]
#define VK_PUSH_CONSTANT [[vk::push_constant]]
#define VK_IMAGE_FORMAT(fmt) [[vk::image_format(#fmt)]]
#else
#define VK_LOCATION(l)
#define VK_BINDING(b, s)
#define VK_PUSH_CONSTANT
#define VK_IMAGE_FORMAT(fmt)
#endif

#endif
