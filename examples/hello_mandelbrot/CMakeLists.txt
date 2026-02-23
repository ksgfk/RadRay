if (RADRAY_ENABLE_DXC)
    add_executable(hello_mandelbrot hello_mandelbrot.cpp)
    target_link_libraries(hello_mandelbrot PRIVATE radraywindow radrayrender radrayimgui)
    target_compile_options(hello_mandelbrot PRIVATE $<$<BOOL:${MSVC}>:/W4>)
    radray_optimize_flags_binary(hello_mandelbrot)
    radray_set_build_path(hello_mandelbrot)

    radray_example_files(hello_mandelbrot
        ${CMAKE_CURRENT_SOURCE_DIR}/mandelbrot.hlsl
        ${CMAKE_CURRENT_SOURCE_DIR}/blit.hlsl)
endif()
