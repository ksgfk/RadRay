add_executable(example_tidal_atrium tidal_atrium.cpp)
target_link_libraries(example_tidal_atrium PRIVATE radrayruntime)
target_compile_definitions(example_tidal_atrium PRIVATE
    RADRAY_ASSETS_DIR_DEFAULT="${CMAKE_SOURCE_DIR}/assets"
    RADRAY_PROJECT_DIR_DEFAULT="${CMAKE_SOURCE_DIR}")
if (RADRAY_ENABLE_SHADER_JIT)
    target_compile_definitions(example_tidal_atrium PRIVATE RADRAY_ENABLE_SHADER_JIT)
    radray_deploy_dxc_runtime(example_tidal_atrium)
endif()
radray_default_compile_flags(example_tidal_atrium)
radray_optimize_flags_binary(example_tidal_atrium)
radray_set_build_path(example_tidal_atrium)
