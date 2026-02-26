if (RADRAY_ENABLE_LIBPNG)
    radray_add_test(test_img_rw
        SOURCES test_img_rw.cpp
        LINK_LIBS radraycore
        DISCOVER_ARGS PROPERTIES ENVIRONMENT "RADRAY_ASSETS_DIR=${CMAKE_SOURCE_DIR}/assets")
endif()
