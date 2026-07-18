file(GLOB_RECURSE RADRAY_RENDER_INC CONFIGURE_DEPENDS "include/*.h")
file(GLOB RADRAY_RENDER_SRC CONFIGURE_DEPENDS "src/*.cpp")
if (RADRAY_ENABLE_VULKAN)
    file(GLOB_RECURSE _VK_SRC CONFIGURE_DEPENDS "src/vk/*.cpp")
    if (APPLE)
        file(GLOB_RECURSE _VK_MM_SRC CONFIGURE_DEPENDS "src/vk/*.mm")
        list(APPEND _VK_SRC ${_VK_MM_SRC})
    endif()
    list(APPEND RADRAY_RENDER_SRC ${_VK_SRC})
endif()
if (RADRAY_ENABLE_D3D12)
    file(GLOB_RECURSE _D3D12_SRC CONFIGURE_DEPENDS "src/d3d12/*.cpp")
    list(APPEND RADRAY_RENDER_SRC ${_D3D12_SRC})
endif()

add_library(radrayrender STATIC ${RADRAY_RENDER_SRC} ${RADRAY_RENDER_INC})
target_include_directories(radrayrender PUBLIC include)
target_link_libraries(radrayrender PUBLIC
    radraycore
    radrayshader
    $<$<BOOL:${RADRAY_ENABLE_VULKAN}>:Vulkan::Headers>
    $<$<BOOL:${RADRAY_ENABLE_VULKAN}>:volk::volk_headers>
    $<$<BOOL:${RADRAY_ENABLE_VULKAN}>:GPUOpen::VulkanMemoryAllocator>
    $<$<BOOL:${RADRAY_ENABLE_D3D12}>:Microsoft::DirectX-Headers>
    $<$<BOOL:${RADRAY_ENABLE_D3D12}>:Microsoft::DirectX-Guids>
    $<$<BOOL:${RADRAY_ENABLE_D3D12}>:D3D12MemoryAllocator>)
if (APPLE AND RADRAY_ENABLE_VULKAN)
    find_library(RADRAY_COCOA_FRAMEWORK Cocoa REQUIRED)
    find_library(RADRAY_QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
    target_link_libraries(radrayrender PUBLIC
        ${RADRAY_COCOA_FRAMEWORK}
        ${RADRAY_QUARTZCORE_FRAMEWORK})
endif()

target_compile_definitions(radrayrender PUBLIC
    $<$<BOOL:${RADRAY_ENABLE_VULKAN}>:RADRAY_ENABLE_VULKAN>
    $<$<BOOL:${RADRAY_ENABLE_D3D12}>:RADRAY_ENABLE_D3D12>)
radray_default_compile_flags(radrayrender)
radray_optimize_flags_library(radrayrender)
radray_set_build_path(radrayrender)

if (RADRAY_BUILD_TESTS)
    add_subdirectory(tests)
endif()
