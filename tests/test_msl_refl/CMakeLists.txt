if (RADRAY_ENABLE_METAL)
    add_executable(test_msl_refl test_msl_refl.mm)
    target_link_libraries(test_msl_refl radraycore radrayrender GTest::gtest_main
        "-framework Metal" "-framework QuartzCore")
    set_source_files_properties(test_msl_refl.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    gtest_discover_tests(test_msl_refl)
    radray_optimize_flags_binary(test_msl_refl)
    radray_set_build_path(test_msl_refl)
endif()
