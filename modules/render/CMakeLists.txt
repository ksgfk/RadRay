file(GLOB_RECURSE RADRAY_RENDER_INC CONFIGURE_DEPENDS "include/*.h")
file(GLOB RADRAY_RENDER_COMMON_SRC CONFIGURE_DEPENDS "src/*.cpp")
if (RADRAY_ENABLE_VULKAN)
    file(GLOB_RECURSE RADRAY_RENDER_VULKAN_SRC CONFIGURE_DEPENDS "src/vk/*.cpp")
endif()

add_library(radrayrender STATIC ${RADRAY_RENDER_COMMON_SRC} ${RADRAY_RENDER_INC}
    $<$<BOOL:${RADRAY_ENABLE_VULKAN}>:${RADRAY_RENDER_VULKAN_SRC}>)
target_include_directories(radrayrender PUBLIC include)
target_link_libraries(radrayrender PUBLIC
    radraycore
    $<$<BOOL:${RADRAY_ENABLE_VULKAN}>:Vulkan::Headers>
    $<$<BOOL:${RADRAY_ENABLE_VULKAN}>:volk::volk>
    $<$<BOOL:${RADRAY_ENABLE_VULKAN}>:GPUOpen::VulkanMemoryAllocator>)
target_link_libraries(radrayrender PRIVATE
    $<$<BOOL:${RADRAY_ENABLE_DXC}>:dxc::dxcompiler>)
target_compile_definitions(radrayrender PUBLIC
    $<$<BOOL:${RADRAY_ENABLE_DXC}>:RADRAY_ENABLE_DXC>
    $<$<BOOL:${RADRAY_ENABLE_VULKAN}>:RADRAY_ENABLE_VULKAN>)
if (MSVC)
    target_compile_options(radrayrender PRIVATE /W4)
    target_compile_options(radrayrender PRIVATE
        $<$<AND:$<CXX_COMPILER_ID:Clang>,$<BOOL:${MSVC}>>:-Wno-nullability-completeness>)
endif()
