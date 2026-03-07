if (RADRAY_ENABLE_DXC)
    radray_add_example(hello_mandelbrot
        SOURCES hello_mandelbrot.cpp
        LINK_LIBS radraywindow radrayrender radrayimgui
        COMPILE_OPTIONS $<$<BOOL:${MSVC}>:/W4>)

    radray_example_files(hello_mandelbrot
        ${CMAKE_CURRENT_SOURCE_DIR}/mandelbrot.hlsl
        ${CMAKE_CURRENT_SOURCE_DIR}/blit.hlsl)
endif()
