set(radray_render_test_sources
    known_gpu_issues.cpp
    test_known_gpu_issues.cpp
    external_resource_test_utils.cpp
    external_texture_test_utils.cpp
    test_external_texture.cpp
    test_external_resource.cpp
    test_attachment_operations.cpp
    test_buffer_mapping.cpp
    test_texture_views.cpp
    test_vulkan_render_objects.cpp
    test_vulkan_device_features.cpp
    test_vulkan_command_contracts.cpp
    test_d3d12_command_contracts.cpp
    test_swapchain_retirement_vulkan.cpp
    test_swapchain_wsi_vulkan.cpp
    test_explicit_binding_layout.cpp
    test_d3d12_diagnostics.cpp)

add_executable(test_radray_render ${radray_render_test_sources})
target_link_libraries(test_radray_render PRIVATE radrayrender GTest::gtest_main)
radray_default_compile_flags(test_radray_render)
radray_optimize_flags_binary(test_radray_render)
radray_set_build_path(test_radray_render)
radray_use_test_shaders(test_radray_render)
gtest_discover_tests(test_radray_render)
target_include_directories(test_radray_render PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_compile_definitions(test_radray_render PRIVATE RADRAY_PROJECT_DIR="${RADRAY_SOURCE_ROOT}")

target_precompile_headers(test_radray_render PRIVATE <radray/render/rhi.h>)

if (WIN32)
    target_sources(test_radray_render PRIVATE external_resource_ipc.cpp)
    add_executable(radray_external_worker external_resource_ipc.cpp external_resource_test_utils.cpp external_texture_test_utils.cpp known_gpu_issues.cpp)
    target_link_libraries(radray_external_worker PRIVATE radrayrender GTest::gtest)
    target_include_directories(radray_external_worker PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    target_compile_definitions(radray_external_worker PRIVATE RADRAY_EXTERNAL_WORKER_MAIN RADRAY_PROJECT_DIR="${RADRAY_SOURCE_ROOT}")
    radray_default_compile_flags(radray_external_worker)
    radray_optimize_flags_binary(radray_external_worker)
    radray_set_build_path(radray_external_worker)
    radray_use_test_shaders(radray_external_worker)
    target_compile_definitions(test_radray_render PRIVATE RADRAY_EXTERNAL_WORKER_PATH="$<TARGET_FILE:radray_external_worker>")
    add_dependencies(test_radray_render radray_external_worker)
endif()
