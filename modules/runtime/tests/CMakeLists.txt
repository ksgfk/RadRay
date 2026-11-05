add_executable(test_radray_runtime
    test_material_tag.cpp
    test_material.cpp
    test_mesh_draw_command.cpp
    test_mesh_pass_processor.cpp
    test_scene.cpp
    test_frame_slot.cpp
    test_frame_scheduler.cpp
    test_gpu_device.cpp)
target_link_libraries(test_radray_runtime PRIVATE RadRay::runtime GTest::gtest_main)
radray_default_compile_flags(test_radray_runtime)
radray_optimize_flags_binary(test_radray_runtime)
radray_set_build_path(test_radray_runtime)
radray_load_mimalloc_early(test_radray_runtime)
gtest_discover_tests(test_radray_runtime)
