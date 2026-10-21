if (NOT RADRAY_BUILD_RUNTIME OR NOT RADRAY_ENABLE_SHADER_JIT)
    message(FATAL_ERROR "RADRAY_BUILD_EXAMPLES requires runtime and shader JIT")
endif()
add_executable(example_scene_sync scene_sync/example_scene_sync.cpp scene_sync/example_scene_sync.h scene_sync/scene_draw.cpp scene_sync/scene_draw.h)
target_link_libraries(example_scene_sync PRIVATE radrayruntime)
target_compile_definitions(example_scene_sync PRIVATE RADRAY_SCENE_EXAMPLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/scene_sync" RADRAY_SHADERLIB_DIR="${CMAKE_SOURCE_DIR}/shaderlib")
radray_default_compile_flags(example_scene_sync)
radray_optimize_flags_binary(example_scene_sync)
radray_set_build_path(example_scene_sync)

add_executable(example_framework_stress framework_stress/example_framework_stress.cpp framework_stress/example_framework_stress.h scene_sync/scene_draw.cpp scene_sync/scene_draw.h)
target_link_libraries(example_framework_stress PRIVATE radrayruntime)
target_include_directories(example_framework_stress PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/scene_sync")
target_compile_definitions(example_framework_stress PRIVATE RADRAY_SCENE_EXAMPLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/scene_sync" RADRAY_SHADERLIB_DIR="${CMAKE_SOURCE_DIR}/shaderlib")
radray_default_compile_flags(example_framework_stress)
radray_optimize_flags_binary(example_framework_stress)
radray_set_build_path(example_framework_stress)

add_executable(example_descriptor_reuse
    descriptor_reuse/example_descriptor_reuse.h descriptor_reuse/example_descriptor_reuse.cpp
    descriptor_reuse/workload.h descriptor_reuse/workload.cpp
    descriptor_reuse/descriptor_probe.h descriptor_reuse/descriptor_probe.cpp
    descriptor_reuse/framework_bridge.h descriptor_reuse/framework_bridge.cpp)
target_link_libraries(example_descriptor_reuse PRIVATE radrayruntime radrayshadercompiler)
target_compile_definitions(example_descriptor_reuse PRIVATE
    RADRAY_DESCRIPTOR_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/descriptor_reuse_shaders"
    RADRAY_DESCRIPTOR_COMPILER_PATH="$<TARGET_FILE:RadRayDXC::Compiler>")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/descriptor_reuse_shaders")
configure_file(descriptor_reuse/reuse.hlsl descriptor_reuse_shaders/reuse.hlsl COPYONLY)
configure_file(descriptor_reuse/depth.hlsl descriptor_reuse_shaders/depth.hlsl COPYONLY)
file(COPY "${CMAKE_SOURCE_DIR}/shaderlib/core" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/descriptor_reuse_shaders/shaderlib")
radray_default_compile_flags(example_descriptor_reuse)
radray_optimize_flags_binary(example_descriptor_reuse)
radray_set_build_path(example_descriptor_reuse)
