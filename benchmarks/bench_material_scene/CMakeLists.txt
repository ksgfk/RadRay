add_executable(bench_material_scene bench_material_scene.cpp bench_material_scene.h material_scene_workload.cpp material_scene_workload.h
    ${CMAKE_SOURCE_DIR}/examples/scene_sync/scene_draw.cpp ${CMAKE_SOURCE_DIR}/examples/scene_sync/scene_draw.h)
target_link_libraries(bench_material_scene PRIVATE radrayruntime benchmark::benchmark)
target_include_directories(bench_material_scene PRIVATE ${CMAKE_SOURCE_DIR}/examples/scene_sync)
target_compile_definitions(bench_material_scene PRIVATE RADRAY_BENCH_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}" RADRAY_SHADERLIB_DIR="${CMAKE_SOURCE_DIR}/shaderlib")
radray_default_compile_flags(bench_material_scene)
radray_optimize_flags_binary(bench_material_scene)
radray_set_build_path(bench_material_scene)
