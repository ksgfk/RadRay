# add_subdirectory(test_hello_world_triangle)
if (RADRAY_BUILD_WINDOW)
    radray_add_test(test_rhi_swapchain SOURCES test_rhi_swapchain.cpp LINK_LIBS radraycore radrayrender radraywindow)
endif()
