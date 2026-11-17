file(GLOB_RECURSE RADRAY_GPU_IMPL_INC CONFIGURE_DEPENDS "include/*.h")
file(GLOB RADRAY_GPU_IMPL_SRC CONFIGURE_DEPENDS "src/*.cpp")
if (RADRAY_ENABLE_VULKAN)
    file(GLOB_RECURSE _VK_SRC CONFIGURE_DEPENDS "src/vk/*.cpp")
    if (APPLE)
        file(GLOB_RECURSE _VK_MM_SRC CONFIGURE_DEPENDS "src/vk/*.mm")
        list(APPEND _VK_SRC ${_VK_MM_SRC})
    endif()
    list(APPEND RADRAY_GPU_IMPL_SRC ${_VK_SRC})
endif()
if (RADRAY_ENABLE_D3D12)
    file(GLOB_RECURSE _D3D12_SRC CONFIGURE_DEPENDS "src/d3d12/*.cpp")
    list(APPEND RADRAY_GPU_IMPL_SRC ${_D3D12_SRC})
endif()

add_library(radraygpu_impl STATIC ${RADRAY_GPU_IMPL_SRC} ${RADRAY_GPU_IMPL_INC})
add_library(RadRay::gpu_impl ALIAS radraygpu_impl)
set_target_properties(radraygpu_impl PROPERTIES EXPORT_NAME gpu_impl)
target_include_directories(radraygpu_impl PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(radraygpu_impl PUBLIC
    RadRay::gpu
    $<$<BOOL:${RADRAY_ENABLE_VULKAN}>:Vulkan::Headers>
    $<$<BOOL:${RADRAY_ENABLE_VULKAN}>:volk::volk_headers>
    $<$<BOOL:${RADRAY_ENABLE_VULKAN}>:GPUOpen::VulkanMemoryAllocator>
    $<$<BOOL:${RADRAY_ENABLE_D3D12}>:Microsoft::DirectX-Headers>
    $<$<BOOL:${RADRAY_ENABLE_D3D12}>:Microsoft::DirectX-Guids>
    $<$<BOOL:${RADRAY_ENABLE_D3D12}>:GPUOpen::D3D12MemoryAllocator>)
target_link_libraries(radraygpu_impl PRIVATE
    xxHash::xxhash)
if (APPLE AND RADRAY_ENABLE_VULKAN)
    find_library(RADRAY_COCOA_FRAMEWORK Cocoa REQUIRED)
    find_library(RADRAY_QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
    target_link_libraries(radraygpu_impl PUBLIC
        "-framework Cocoa"
        "-framework QuartzCore")
endif()

target_compile_definitions(radraygpu_impl PUBLIC
    $<$<BOOL:${RADRAY_ENABLE_VULKAN}>:RADRAY_ENABLE_VULKAN>
    $<$<BOOL:${RADRAY_ENABLE_D3D12}>:RADRAY_ENABLE_D3D12>)
radray_default_compile_flags(radraygpu_impl)
radray_optimize_flags_library(radraygpu_impl)
radray_set_build_path(radraygpu_impl)
if (APPLE AND RADRAY_ENABLE_VULKAN)
    set_source_files_properties(${_VK_MM_SRC} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endif()
target_precompile_headers(radraygpu_impl PRIVATE <radray/gpu/types.h>)

if (RADRAY_BUILD_TESTS)
    add_subdirectory(tests)
endif()
