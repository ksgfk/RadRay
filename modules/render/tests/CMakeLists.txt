# RenderPassCacheKey / FramebufferCacheKey 的纯 CPU 行为 (相等性、哈希、descriptor 回读),
# 不需要 device。RenderPassRegistry 本身要真设备, 由 runtime 侧的集成测试覆盖。
# D3D12 device loss 用 EXPECT_EXIT，RemoveDevice 发生在子进程，可以和其它 render 用例链在一起。
set(radray_render_test_sources
    test_render_pass_registry.cpp
    test_pipeline_layout_cache.cpp
    test_device_capabilities.cpp
    test_gpu_test_fixture.cpp
    test_radray_render_shader_artifact.cpp
    test_radray_shader_contract.cpp
    test_radray_render_shader_layout.cpp
    test_radray_render_vulkan_layout.cpp
    test_radray_render_d3d12_layout.cpp
    test_d3d12_device_loss.cpp)

radray_add_test(test_radray_render
    SOURCES ${radray_render_test_sources}
    LINK_LIBS radrayrender radrayshader)
target_include_directories(test_radray_render PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_compile_definitions(test_radray_render PRIVATE RADRAY_PROJECT_DIR="${CMAKE_SOURCE_DIR}")

if (RADRAY_BUILD_SHADER_COMPILER)
    add_executable(radray_shader_fixture_generator shader_fixture_generator.cpp)
    target_compile_definitions(radray_shader_fixture_generator PRIVATE RADRAY_PROJECT_DIR="${CMAKE_SOURCE_DIR}")
    target_link_libraries(radray_shader_fixture_generator PRIVATE radrayshadercompiler)
    radray_default_compile_flags(radray_shader_fixture_generator)
    radray_set_build_path(radray_shader_fixture_generator)
    radray_deploy_dxc_runtime(radray_shader_fixture_generator)

    target_sources(test_radray_render PRIVATE test_radray_render_pso_smoke.cpp)
    target_link_libraries(test_radray_render PRIVATE RadRayDXC::Headers)
    radray_deploy_dxc_runtime(test_radray_render)
endif()

target_precompile_headers(test_radray_render PRIVATE <radray/render/rhi.h>)
