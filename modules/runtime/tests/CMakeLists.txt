add_executable(test_radray_runtime
    test_pipeline_layout_registry.cpp
    test_pso_cache.cpp
    test_asset_manager.cpp
    test_shader_asset.cpp
    test_shader_vertex_input.cpp
    test_shader_layout_gpu.cpp
    test_material_tag.cpp
    test_material.cpp
    test_mesh_draw_command.cpp
    test_mesh_pass_processor.cpp
    test_scene.cpp
    test_frame_slot.cpp
    test_transient_parameter_pool.cpp
    test_frame_scheduler.cpp
    test_geometry_buffer_pool.cpp
    test_upload_arena.cpp
    test_rdg_upload_gpu.cpp
    test_geometry_upload_gpu.cpp
    test_mdi_gpu.cpp
    ../../gpu_impl/tests/gpu_test_fixture.cpp
    test_rdg.cpp
    test_readback_queue.cpp
    test_gpu_device.cpp)
if (RADRAY_BUILD_EXAMPLES)
    target_sources(test_radray_runtime PRIVATE test_shader_publish.cpp
        ${RADRAY_SOURCE_ROOT}/examples/mesh_drawing_pipeline/gpu_support.cpp
        ${RADRAY_SOURCE_ROOT}/examples/mesh_drawing_pipeline/pipeline.cpp)
    target_compile_definitions(test_radray_runtime PRIVATE RADRAY_SAMPLE_ROOT="${RADRAY_SOURCE_ROOT}")
endif()
target_link_libraries(test_radray_runtime PRIVATE RadRay::runtime GTest::gtest_main)
target_compile_definitions(test_radray_runtime PRIVATE RADRAY_SHADERLIB_DIR="${RADRAY_SOURCE_ROOT}/shaderlib")
radray_default_compile_flags(test_radray_runtime)
radray_optimize_flags_binary(test_radray_runtime)
radray_set_build_path(test_radray_runtime)
radray_load_mimalloc_early(test_radray_runtime)
radray_use_test_shaders(test_radray_runtime)
gtest_discover_tests(test_radray_runtime)

if (NOT RADRAY_ENABLE_MIMALLOC)
    # 独立进程映像替换全局 operator new/delete，用于 O1/O2/M2 零堆分配验收。
    add_executable(test_radray_runtime_allocations test_runtime_allocations.cpp)
    target_link_libraries(test_radray_runtime_allocations PRIVATE RadRay::runtime GTest::gtest_main)
    radray_default_compile_flags(test_radray_runtime_allocations)
    radray_optimize_flags_binary(test_radray_runtime_allocations)
    radray_set_build_path(test_radray_runtime_allocations)
    gtest_discover_tests(test_radray_runtime_allocations)
endif()
