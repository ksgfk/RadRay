radray_add_test(test_imgui_rendering SOURCES test_imgui_rendering.cpp LINK_LIBS radrayimgui)
target_include_directories(test_imgui_rendering PRIVATE
    "${CMAKE_SOURCE_DIR}/modules/render/tests"
    "${CMAKE_SOURCE_DIR}/modules/runtime/tests")
