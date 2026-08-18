add_executable(example_lambert_sphere example_lambert_sphere.cpp)

target_link_libraries(example_lambert_sphere PRIVATE radrayruntime)
target_compile_definitions(example_lambert_sphere PRIVATE
    RADRAY_ASSETS_DIR_DEFAULT="${CMAKE_SOURCE_DIR}/assets"
    RADRAY_PROJECT_DIR_DEFAULT="${CMAKE_SOURCE_DIR}")
if (RADRAY_ENABLE_SHADER_JIT)
    target_compile_definitions(example_lambert_sphere PRIVATE RADRAY_ENABLE_SHADER_JIT)
    radray_deploy_dxc_runtime(example_lambert_sphere)
endif()

radray_default_compile_flags(example_lambert_sphere)
radray_optimize_flags_binary(example_lambert_sphere)
radray_set_build_path(example_lambert_sphere)
