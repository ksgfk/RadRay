if (RADRAY_ENABLE_METAL)
    radray_add_test(test_msl_refl
        SOURCES test_msl_refl.mm
        LINK_LIBS radraycore radrayrender "-framework Metal" "-framework QuartzCore"
        COMPILE_OPTIONS -fobjc-arc)
endif()
