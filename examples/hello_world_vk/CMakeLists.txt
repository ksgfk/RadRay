radray_add_example(hello_world_vk
    SOURCES hello_world_vk.cpp
    LINK_LIBS radraywindow radrayrender
    COMPILE_OPTIONS $<$<BOOL:${MSVC}>:/W4>)
