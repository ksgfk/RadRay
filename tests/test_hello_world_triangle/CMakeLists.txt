if (WIN32 AND RADRAY_ENABLE_D3D12 AND RADRAY_ENABLE_DXC AND RADRAY_ENABLE_LIBPNG)
    add_executable(test_hello_world_triangle test_hello_world_triangle.cpp)
    target_link_libraries(test_hello_world_triangle PRIVATE radraycore radrayrender GTest::gtest_main)
    add_test(
        NAME HelloWorldTriangle.D3D12
        COMMAND test_hello_world_triangle --gtest_filter=HelloWorldTriangle.D3D12 --gtest_also_run_disabled_tests)
    set_tests_properties(
        HelloWorldTriangle.D3D12
        PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" ENVIRONMENT "RADRAY_PROJECT_DIR=${CMAKE_SOURCE_DIR}")
    set_property(TEST HelloWorldTriangle.D3D12 APPEND PROPERTY ENVIRONMENT "RADRAY_TEST_ENV_DIR=${CMAKE_CURRENT_SOURCE_DIR}")
    set_property(TEST HelloWorldTriangle.D3D12 APPEND PROPERTY ENVIRONMENT "RADRAY_ASSETS_DIR=${CMAKE_SOURCE_DIR}/assets")
    set_property(TEST HelloWorldTriangle.D3D12 APPEND PROPERTY ENVIRONMENT "RADRAY_TEST_ARTIFACTS_DIR=${CMAKE_CURRENT_SOURCE_DIR}")
    # set_property(TEST HelloWorldTriangle.D3D12 APPEND PROPERTY ENVIRONMENT "RADRAY_TEST_UPDATE_BASELINE=1")
    radray_optimize_flags_binary(test_hello_world_triangle)
    radray_set_build_path(test_hello_world_triangle)
endif()
