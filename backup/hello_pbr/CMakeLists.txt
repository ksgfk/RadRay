if (RADRAY_ENABLE_DXC)
    radray_add_example(hello_pbr
        SOURCES hello_pbr.cpp
        LINK_LIBS radraywindow radrayrender radrayimgui
        COMPILE_OPTIONS $<$<BOOL:${MSVC}>:/W4>)

    radray_example_files(hello_pbr ${CMAKE_CURRENT_SOURCE_DIR}/pbr.hlsl)
endif()
