if (RADRAY_ENABLE_DXC AND RADRAY_ENABLE_SPVCROSS)
    radray_add_test(test_spvc_to_msl
        SOURCES test_spvc_to_msl.cpp
        LINK_LIBS radraycore radrayrender)
endif()
