if (RADRAY_ENABLE_DXC AND RADRAY_ENABLE_SPVCROSS)
    radray_add_test(test_spvc
        SOURCES test_spvc.cpp
        LINK_LIBS radraycore radrayrender)
endif()
