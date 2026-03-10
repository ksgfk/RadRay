file(GLOB_RECURSE RADRAY_RENDER_INC CONFIGURE_DEPENDS "include/*.h")
file(GLOB RADRAY_RENDER_SRC CONFIGURE_DEPENDS "src/*.cpp" "src/shader/*.cpp")
if (RADRAY_ENABLE_DXC)
    list(APPEND RADRAY_RENDER_SRC "src/shader_compiler/dxc.cpp")
endif()
if (RADRAY_ENABLE_SPVCROSS)
    list(APPEND RADRAY_RENDER_SRC "src/shader_compiler/spvc.cpp")
endif()
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
    $<$<BOOL:${RADRAY_ENABLE_VULKAN}>:Vulkan::Headers>
    $<$<BOOL:${RADRAY_ENABLE_VULKAN}>:volk::volk_headers>
    $<$<BOOL:${RADRAY_ENABLE_VULKAN}>:GPUOpen::VulkanMemoryAllocator>
    $<$<BOOL:${RADRAY_ENABLE_D3D12}>:Microsoft::DirectX-Headers>
    $<$<BOOL:${RADRAY_ENABLE_D3D12}>:Microsoft::DirectX-Guids>
    $<$<BOOL:${RADRAY_ENABLE_D3D12}>:D3D12MemoryAllocator>)
target_link_libraries(radrayrender PRIVATE
    $<$<BOOL:${RADRAY_ENABLE_DXC}>:dxc::dxcompiler>
    $<$<BOOL:${RADRAY_ENABLE_SPVCROSS}>:spirv-cross-core>
    $<$<BOOL:${RADRAY_ENABLE_SPVCROSS}>:spirv-cross-glsl>
    $<$<BOOL:${RADRAY_ENABLE_SPVCROSS}>:spirv-cross-msl>
    $<$<BOOL:${RADRAY_ENABLE_SPVCROSS}>:spirv-cross-util>)

target_compile_definitions(radrayrender PUBLIC
    $<$<BOOL:${RADRAY_ENABLE_DXC}>:RADRAY_ENABLE_DXC>
    $<$<BOOL:${RADRAY_ENABLE_VULKAN}>:RADRAY_ENABLE_VULKAN>
    $<$<BOOL:${RADRAY_ENABLE_D3D12}>:RADRAY_ENABLE_D3D12>
    $<$<BOOL:${RADRAY_ENABLE_SPVCROSS}>:RADRAY_ENABLE_SPIRV_CROSS>)
radray_default_compile_flags(radrayrender)
radray_optimize_flags_library(radrayrender)
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

if (RADRAY_BUILD_TESTS)
    add_subdirectory(tests)
endif()
