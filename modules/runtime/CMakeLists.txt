file(GLOB_RECURSE RADRAY_RUNTIME_INC CONFIGURE_DEPENDS "include/*.h")
file(GLOB_RECURSE RADRAY_RUNTIME_SRC CONFIGURE_DEPENDS "src/*.cpp")

add_library(radrayruntime STATIC ${RADRAY_RUNTIME_SRC} ${RADRAY_RUNTIME_INC})
target_include_directories(radrayruntime PUBLIC include)
option(RADRAY_RUNTIME_DETAILED_PROFILING "Enable per-command and per-group runtime CPU zones for dedicated captures" OFF)
if (RADRAY_RUNTIME_DETAILED_PROFILING)
    target_compile_definitions(radrayruntime PUBLIC RADRAY_RUNTIME_DETAILED_PROFILING)
endif()
target_link_libraries(radrayruntime PUBLIC
    radraycore
    radrayshader
    radrayrender
    radraywindow)
target_link_libraries(radrayruntime PRIVATE cgltf::cgltf)
if (RADRAY_ENABLE_SHADER_JIT)
    target_compile_definitions(radrayruntime PRIVATE RADRAY_ENABLE_SHADER_JIT)
    target_link_libraries(radrayruntime PUBLIC radrayshadercompiler)
endif()
if (WIN32 AND (RADRAY_ENABLE_D3D12 OR RADRAY_ENABLE_VULKAN))
    target_link_libraries(radrayruntime PRIVATE dxgi)
endif()
radray_default_compile_flags(radrayruntime)
radray_optimize_flags_library(radrayruntime)
radray_set_build_path(radrayruntime)

if (RADRAY_BUILD_TESTS)
    add_subdirectory(tests)
endif()
