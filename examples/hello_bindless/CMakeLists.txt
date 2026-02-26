if (RADRAY_ENABLE_DXC)
    radray_add_example(hello_bindless
        SOURCES hello_bindless.cpp
        LINK_LIBS radraywindow radrayrender radrayimgui
        COMPILE_OPTIONS $<$<BOOL:${MSVC}>:/W4>)

    radray_example_files(hello_bindless ${CMAKE_CURRENT_SOURCE_DIR}/bindless.hlsl)
endif()
