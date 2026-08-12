radray_add_test(test_asset_slot SOURCES test_asset_slot.cpp LINK_LIBS radrayruntime)

if (RADRAY_ENABLE_SHADER_JIT)
    radray_add_test(test_runtime_shader_jit SOURCES test_runtime_shader_jit.cpp LINK_LIBS radrayruntime)
    target_include_directories(test_runtime_shader_jit PRIVATE
        "${CMAKE_SOURCE_DIR}/modules/render/tests")
    target_link_libraries(test_runtime_shader_jit PRIVATE RadRayDXC::Headers)
    target_compile_definitions(test_runtime_shader_jit PRIVATE RADRAY_PROJECT_DIR="${CMAKE_SOURCE_DIR}")
endif()
