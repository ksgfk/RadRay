if (RADRAY_ENABLE_DXC AND RADRAY_ENABLE_SPVCROSS)
    radray_add_test(test_hlsl_to_msl_refl
        SOURCES test_hlsl_to_msl_refl.cpp
        LINK_LIBS radraycore radrayrender)
endif()
