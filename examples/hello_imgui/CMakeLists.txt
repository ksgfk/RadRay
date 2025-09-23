add_executable(hello_imgui hello_imgui.cpp)
target_link_libraries(hello_imgui PRIVATE radraywindow radrayrender)
target_compile_options(hello_imgui PRIVATE $<$<BOOL:MSVC>:/W4>)
radray_set_build_path(hello_imgui)
