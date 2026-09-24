radray_add_test(
    test_radray_shader_compiler
    SOURCES
        test_radray_shader_compiler_client.cpp
        test_radray_dxc_metadata.cpp
        test_shaderlib_passes.cpp
    LINK_LIBS radrayshadercompiler)
target_link_libraries(test_radray_shader_compiler PRIVATE RadRayDXC::Headers)
radray_deploy_dxc_runtime(test_radray_shader_compiler)
target_compile_definitions(test_radray_shader_compiler PRIVATE RADRAY_PROJECT_DIR="${CMAKE_SOURCE_DIR}")
