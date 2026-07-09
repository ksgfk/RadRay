radray_add_example(example_imgui SOURCES example_imgui.cpp LINK_LIBS radraycore radrayrender radrayruntime radraywindow)
target_compile_definitions(example_imgui PRIVATE "RADRAY_EXAMPLE_ASSET_DIR=\"${CMAKE_SOURCE_DIR}/assets/example_imgui\"")
