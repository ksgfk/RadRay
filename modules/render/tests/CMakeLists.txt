set(radray_render_test_sources
    test_pipeline_layout_cache.cpp
    test_swapchain_retirement_vulkan.cpp
    test_swapchain_wsi_vulkan.cpp
    test_radray_render_shader_artifact.cpp
    test_radray_shader_contract.cpp
    test_radray_render_shader_layout.cpp
    test_radray_render_vulkan_layout.cpp
    test_radray_render_d3d12_layout.cpp)

add_executable(test_radray_render ${radray_render_test_sources})
target_link_libraries(test_radray_render PRIVATE radrayrender radrayshader GTest::gtest_main)
radray_default_compile_flags(test_radray_render)
radray_optimize_flags_binary(test_radray_render)
radray_set_build_path(test_radray_render)
gtest_discover_tests(test_radray_render)
target_include_directories(test_radray_render PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_compile_definitions(test_radray_render PRIVATE RADRAY_PROJECT_DIR="${RADRAY_SOURCE_ROOT}")

if (RADRAY_BUILD_SHADER_COMPILER)
    add_executable(radray_shader_fixture_generator shader_fixture_generator.cpp)
    target_compile_definitions(radray_shader_fixture_generator PRIVATE RADRAY_PROJECT_DIR="${RADRAY_SOURCE_ROOT}")
    target_link_libraries(radray_shader_fixture_generator PRIVATE radrayshadercompiler)
    radray_default_compile_flags(radray_shader_fixture_generator)
    radray_set_build_path(radray_shader_fixture_generator)
endif()

target_precompile_headers(test_radray_render PRIVATE <radray/render/rhi.h>)
