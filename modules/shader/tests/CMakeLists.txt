add_executable(test_radray_shader
    test_diagnostic_result.cpp
    test_shader_compiler_contract.cpp
    test_shader_metadata_wire.cpp
    test_shader_artifact.cpp
    test_client.cpp
    test_contract_discovery.cpp
    test_dxc_metadata.cpp
    test_shaderlib_passes.cpp
    test_shader_reflector.cpp
    test_shader_layout.cpp)
target_link_libraries(test_radray_shader PRIVATE RadRay::shader GTest::gtest_main)
target_compile_definitions(test_radray_shader PRIVATE RADRAY_PROJECT_DIR="${RADRAY_SOURCE_ROOT}")
radray_default_compile_flags(test_radray_shader)
radray_optimize_flags_binary(test_radray_shader)
radray_set_build_path(test_radray_shader)
radray_load_mimalloc_early(test_radray_shader)
radray_use_test_shaders(test_radray_shader)
if (RADRAY_BUILD_SHADER_REFLECTOR_TOOLS)
    target_sources(test_radray_shader PRIVATE test_shader_reflect_cli.cpp ../../../tools/shader_reflect/shader_reflect.cpp)
    target_compile_definitions(test_radray_shader PRIVATE RADRAY_SHADER_REFLECT_NO_MAIN)
endif()
gtest_discover_tests(test_radray_shader)

add_executable(radray_shader_fixture_generator EXCLUDE_FROM_ALL shader_fixture_generator.cpp)
target_compile_definitions(radray_shader_fixture_generator PRIVATE RADRAY_PROJECT_DIR="${RADRAY_SOURCE_ROOT}")
target_link_libraries(radray_shader_fixture_generator PRIVATE RadRay::shader)
radray_default_compile_flags(radray_shader_fixture_generator)
radray_set_build_path(radray_shader_fixture_generator)
radray_load_mimalloc_early(radray_shader_fixture_generator)
