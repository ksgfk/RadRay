if (WIN32)
    radray_add_test(test_radray_window
        SOURCES test_event_pump.cpp
        LINK_LIBS radraywindow
        DISCOVER_ARGS PROPERTIES RUN_SERIAL TRUE TIMEOUT 15)
endif()
