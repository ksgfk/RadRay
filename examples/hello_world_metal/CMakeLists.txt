radray_add_example(hello_world_metal
    SOURCES hello_world_metal.cpp
    LINK_LIBS radraywindow radrayrender
    COMPILE_OPTIONS $<$<BOOL:${MSVC}>:/W4>)
