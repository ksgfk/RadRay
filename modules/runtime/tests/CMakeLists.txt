add_executable(test_radray_runtime test_mesh_pipeline.cpp test_graph.cpp test_runtime_gpu.cpp)
target_link_libraries(test_radray_runtime PRIVATE RadRay::runtime GTest::gtest_main)
target_include_directories(test_radray_runtime PRIVATE
    "${RADRAY_SOURCE_ROOT}/modules/render/tests" "${RADRAY_SOURCE_ROOT}/modules/render/src")
radray_default_compile_flags(test_radray_runtime)
radray_optimize_flags_binary(test_radray_runtime)
radray_set_build_path(test_radray_runtime)

set(RADRAY_RUNTIME_TEST_SHADER_DIR "${RADRAY_SOURCE_ROOT}/build/runtime-test-shaders" CACHE PATH "Offline runtime test bytecode")
target_compile_definitions(test_radray_runtime PRIVATE RADRAY_RUNTIME_TEST_SHADER_DIR="${RADRAY_RUNTIME_TEST_SHADER_DIR}")
add_custom_target(radray_check_runtime_test_shaders
    COMMAND "${Python3_EXECUTABLE}" "${RADRAY_SOURCE_ROOT}/tools/compile_runtime_test_shaders.py"
        --check --output-dir "${RADRAY_RUNTIME_TEST_SHADER_DIR}"
    VERBATIM)
add_dependencies(test_radray_runtime radray_check_runtime_test_shaders)
gtest_discover_tests(test_radray_runtime PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
