if (RADRAY_ENABLE_DXC AND RADRAY_ENABLE_SPVCROSS)
    radray_add_test(test_bind_bridge
        SOURCES test_bind_bridge.cpp
        LINK_LIBS radraycore radrayrender)
endif()
