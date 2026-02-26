if (RADRAY_BUILD_WINDOW)
    radray_add_test(test_native_window
        SOURCES test_native_window.cpp
        LINK_LIBS radraywindow)
endif()
