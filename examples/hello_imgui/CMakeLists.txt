radray_add_example(hello_imgui
    SOURCES hello_imgui.cpp
    LINK_LIBS radraywindow radrayrender radrayimgui
    COMPILE_OPTIONS $<$<BOOL:${MSVC}>:/W4>)
