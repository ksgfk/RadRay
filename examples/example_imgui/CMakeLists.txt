radray_add_example(example_imgui SOURCES example_imgui.cpp LINK_LIBS radraycore radrayrender radrayruntime radraywindow)
radray_example_files(example_imgui sphere.hlsl)
# radray_example_files copies sources into ${CMAKE_SOURCE_DIR}/assets/<target>.
# Hand that directory to the binary so it can locate shaders no matter the working dir.
target_compile_definitions(example_imgui PRIVATE "RADRAY_EXAMPLE_ASSET_DIR=\"${CMAKE_SOURCE_DIR}/assets/example_imgui\"")
