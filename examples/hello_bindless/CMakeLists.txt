if (RADRAY_ENABLE_DXC)
    add_executable(hello_bindless hello_bindless.cpp)
    target_link_libraries(hello_bindless PRIVATE radraywindow radrayrender radrayimgui)
    target_compile_options(hello_bindless PRIVATE $<$<BOOL:${MSVC}>:/W4>)
    radray_optimize_flags_binary(hello_bindless)
    radray_set_build_path(hello_bindless)

    radray_example_files(hello_bindless ${CMAKE_CURRENT_SOURCE_DIR}/bindless.hlsl)
endif()
