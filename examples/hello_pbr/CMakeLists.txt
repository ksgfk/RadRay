if (RADRAY_ENABLE_DXC)
    add_executable(hello_pbr hello_pbr.cpp)
    target_link_libraries(hello_pbr PRIVATE radraywindow radrayrender radrayimgui)
    target_compile_options(hello_pbr PRIVATE $<$<BOOL:MSVC>:/W4>)
    radray_optimize_flags_binary(hello_pbr)
    radray_set_build_path(hello_pbr)

    radray_example_files(hello_pbr ${CMAKE_CURRENT_SOURCE_DIR}/pbr.hlsl)
endif()
