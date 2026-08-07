# RenderPassCacheKey / FramebufferCacheKey 的纯 CPU 行为 (相等性、哈希、descriptor 回读),
# 不需要 device。RenderPassRegistry 本身要真设备, 由 runtime 侧的集成测试覆盖。
radray_add_test(test_render_pass_registry SOURCES test_render_pass_registry.cpp LINK_LIBS radrayrender)
radray_add_test(test_radray_render_shader_artifact SOURCES test_radray_render_shader_artifact.cpp LINK_LIBS radrayrender)
target_compile_definitions(test_radray_render_shader_artifact PRIVATE RADRAY_PROJECT_DIR="${CMAKE_SOURCE_DIR}")
radray_add_test(test_radray_shader_contract SOURCES test_radray_shader_contract.cpp LINK_LIBS radraycore radrayshader)
target_compile_definitions(test_radray_shader_contract PRIVATE RADRAY_PROJECT_DIR="${CMAKE_SOURCE_DIR}")

if (RADRAY_BUILD_SHADER_COMPILER)
    add_executable(radray_shader_fixture_generator shader_fixture_generator.cpp)
    target_compile_definitions(radray_shader_fixture_generator PRIVATE RADRAY_PROJECT_DIR="${CMAKE_SOURCE_DIR}")
    target_link_libraries(radray_shader_fixture_generator PRIVATE radrayshadercompiler)
    radray_default_compile_flags(radray_shader_fixture_generator)
    radray_set_build_path(radray_shader_fixture_generator)
    radray_deploy_dxc_runtime(radray_shader_fixture_generator)

    radray_add_test(test_radray_render_pso_smoke SOURCES test_radray_render_pso_smoke.cpp LINK_LIBS radrayrender)
    target_link_libraries(test_radray_render_pso_smoke PRIVATE RadRayDXC::Headers)
    radray_deploy_dxc_runtime(test_radray_render_pso_smoke)
endif()
