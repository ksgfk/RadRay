add_executable(test_radray_gpu
    test_enums.cpp
    test_descriptors.cpp
    test_rhi.cpp)
target_link_libraries(test_radray_gpu PRIVATE RadRay::gpu GTest::gtest_main)
radray_default_compile_flags(test_radray_gpu)
radray_optimize_flags_binary(test_radray_gpu)
radray_set_build_path(test_radray_gpu)
radray_load_mimalloc_early(test_radray_gpu)
# Keep separate translation units to detect header ODR/linkage regressions.
set_target_properties(test_radray_gpu PROPERTIES UNITY_BUILD OFF)
gtest_discover_tests(test_radray_gpu)
