radray_add_example(hello_world_rt_d3d12
    SOURCES hello_world_rt_d3d12.cpp
    LINK_LIBS radraywindow radrayrender
    COMPILE_OPTIONS $<$<BOOL:${MSVC}>:/W4>)
