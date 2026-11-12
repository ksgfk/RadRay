add_executable(test_radray_render
    gpu_test_fixture.cpp
    test_device.cpp
    test_buffer.cpp
    test_compute_binding.cpp
    test_render_pass.cpp
    test_texture_view.cpp
    test_external_resource.cpp
    test_vulkan_device_features.cpp
    test_swapchain.cpp
    test_vulkan_swapchain.cpp)
target_link_libraries(test_radray_render PRIVATE radrayrender GTest::gtest_main)
radray_default_compile_flags(test_radray_render)
radray_optimize_flags_binary(test_radray_render)
radray_set_build_path(test_radray_render)
radray_load_mimalloc_early(test_radray_render)
radray_use_test_shaders(test_radray_render)
target_precompile_headers(test_radray_render PRIVATE <radray/render/rhi.h>)
gtest_discover_tests(test_radray_render)
