set(radray_render_test_sources
    test_swapchain_retirement_vulkan.cpp
    test_swapchain_wsi_vulkan.cpp
    test_explicit_binding_layout.cpp)

add_executable(test_radray_render ${radray_render_test_sources})
target_link_libraries(test_radray_render PRIVATE radrayrender GTest::gtest_main)
radray_default_compile_flags(test_radray_render)
radray_optimize_flags_binary(test_radray_render)
radray_set_build_path(test_radray_render)
gtest_discover_tests(test_radray_render)
target_include_directories(test_radray_render PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_compile_definitions(test_radray_render PRIVATE RADRAY_PROJECT_DIR="${RADRAY_SOURCE_ROOT}")

target_precompile_headers(test_radray_render PRIVATE <radray/render/rhi.h>)
