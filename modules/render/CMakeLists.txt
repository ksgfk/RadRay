file(GLOB_RECURSE RADRAY_RENDER_INC CONFIGURE_DEPENDS "include/*.h")
file(GLOB RADRAY_RENDER_COMMON_SRC CONFIGURE_DEPENDS "src/*.cpp")
if (RADRAY_ENABLE_VULKAN)
    file(GLOB_RECURSE RADRAY_RENDER_VULKAN_SRC CONFIGURE_DEPENDS "src/vk/*.cpp")
endif()
if (RADRAY_ENABLE_D3D12)
    file(GLOB_RECURSE RADRAY_RENDER_D3D12_SRC CONFIGURE_DEPENDS "src/d3d12/*.cpp")
endif()

add_library(radrayrender STATIC ${RADRAY_RENDER_COMMON_SRC} ${RADRAY_RENDER_INC}
    $<$<BOOL:${RADRAY_ENABLE_VULKAN}>:${RADRAY_RENDER_VULKAN_SRC}>
    $<$<BOOL:${RADRAY_ENABLE_D3D12}>:${RADRAY_RENDER_D3D12_SRC}>)
target_include_directories(radrayrender PUBLIC include)
target_link_libraries(radrayrender PUBLIC
    radraycore
    $<$<BOOL:${RADRAY_ENABLE_VULKAN}>:Vulkan::Headers>
    $<$<BOOL:${RADRAY_ENABLE_VULKAN}>:volk::volk_headers>
    $<$<BOOL:${RADRAY_ENABLE_VULKAN}>:GPUOpen::VulkanMemoryAllocator>
    $<$<OR:$<BOOL:${RADRAY_ENABLE_DXC}>,$<BOOL:${RADRAY_ENABLE_D3D12}>>:Microsoft::DirectX-Headers>
    $<$<BOOL:${RADRAY_ENABLE_D3D12}>:Microsoft::DirectX-Guids>
    $<$<BOOL:${RADRAY_ENABLE_D3D12}>:D3D12MemoryAllocator>
    $<$<BOOL:${RADRAY_ENABLE_IMGUI}>:radrayimgui>)
target_link_libraries(radrayrender PRIVATE
    $<$<BOOL:${RADRAY_ENABLE_DXC}>:dxc::dxcompiler>)
target_compile_definitions(radrayrender PUBLIC
    $<$<BOOL:${RADRAY_ENABLE_DXC}>:RADRAY_ENABLE_DXC>
    $<$<BOOL:${RADRAY_ENABLE_VULKAN}>:RADRAY_ENABLE_VULKAN>
    $<$<BOOL:${RADRAY_ENABLE_D3D12}>:RADRAY_ENABLE_D3D12>)
target_compile_options(radrayrender PRIVATE
    $<$<BOOL:MSVC>:/W4>)
radray_set_build_path(radrayrender)
if (RADRAY_ENABLE_DXC)
    # 构建后复制 dxc 运行时到统一输出目录
    if (DEFINED RADRAY_DXC_RUNTIME_DLLS)
        set(_DXC_RUNTIME_DLLS ${RADRAY_DXC_RUNTIME_DLLS})
    else()
        set(_DXC_RUNTIME_DLLS "$<TARGET_FILE:dxc::dxcompiler>")
    endif()
    set(_DXC_COPY_CMDS COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:radrayrender>")
    foreach(_dll IN LISTS _DXC_RUNTIME_DLLS)
        list(APPEND _DXC_COPY_CMDS
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_dll}" "$<TARGET_FILE_DIR:radrayrender>")
    endforeach()
    add_custom_command(TARGET radrayrender POST_BUILD
        ${_DXC_COPY_CMDS}
        COMMENT "Copy DXC runtime libraries to radrayrender output directory")
    unset(_DXC_COPY_CMDS)
    unset(_DXC_RUNTIME_DLLS)
endif()
