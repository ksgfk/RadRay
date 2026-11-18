include_guard(GLOBAL)

set(RADRAY_TEST_SHADER_DIR "${RADRAY_SOURCE_ROOT}/build/test_shaders" CACHE PATH
    "Directory of locally generated test shader fixtures")

function(radray_use_test_shaders target)
    target_compile_definitions(${target} PRIVATE RADRAY_TEST_SHADER_DIR="${RADRAY_TEST_SHADER_DIR}")
endfunction()
