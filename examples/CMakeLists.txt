if (RADRAY_ENABLE_IMGUI)
    radray_add_example(example_imgui SOURCES example_imgui.cpp LINK_LIBS radraycore radrayrender radrayruntime radraywindow)
endif()
