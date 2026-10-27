include_guard(GLOBAL)

set(RADRAY_TEST_SHADER_DIR "${RADRAY_SOURCE_ROOT}/build/test_shaders" CACHE PATH
    "Directory of locally generated test shader fixtures")
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

# Explicit preparation keeps DXC out of compiler-disabled builds and test processes.
add_custom_target(radray_check_test_shaders
    COMMAND "${Python3_EXECUTABLE}" "${RADRAY_SOURCE_ROOT}/tools/compile_test_shaders.py"
        --check --output-dir "${RADRAY_TEST_SHADER_DIR}"
    COMMENT "Checking local test shader fixtures (run tools/compile_test_shaders.py to generate)"
    VERBATIM)

function(radray_use_test_shaders target)
    target_compile_definitions(${target} PRIVATE RADRAY_TEST_SHADER_DIR="${RADRAY_TEST_SHADER_DIR}")
    add_dependencies(${target} radray_check_test_shaders)
endfunction()
