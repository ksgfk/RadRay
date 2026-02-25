if (WIN32 AND RADRAY_ENABLE_D3D12 AND RADRAY_ENABLE_DXC AND RADRAY_ENABLE_LIBPNG)
    add_executable(test_hello_world_d3d12_offscreen test_hello_world_d3d12_offscreen.cpp)
    target_link_libraries(test_hello_world_d3d12_offscreen PRIVATE radraycore radrayrender GTest::gtest_main)
    gtest_discover_tests(
        test_hello_world_d3d12_offscreen
        PROPERTIES ENVIRONMENT "RADRAY_SOURCE_DIR=${CMAKE_SOURCE_DIR}")
    radray_optimize_flags_binary(test_hello_world_d3d12_offscreen)
    radray_set_build_path(test_hello_world_d3d12_offscreen)
endif()

