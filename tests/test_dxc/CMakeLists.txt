if (RADRAY_ENABLE_DXC)
    radray_add_test(test_dxc
        SOURCES test_dxc.cpp
        LINK_LIBS radraycore radrayrender)
endif()
