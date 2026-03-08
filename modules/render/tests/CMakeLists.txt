radray_add_test(test_binding_layout_builder SOURCES test_binding_layout_builder.cpp LINK_LIBS radraycore radrayrender)
if (RADRAY_ENABLE_D3D12)
    radray_add_test(test_d3d12_binding_layout_builder SOURCES test_d3d12_binding_layout_builder.cpp LINK_LIBS radraycore radrayrender)
endif()
if (RADRAY_ENABLE_VULKAN)
    radray_add_test(test_vulkan_binding_layout_builder SOURCES test_vulkan_binding_layout_builder.cpp LINK_LIBS radraycore radrayrender)
endif()
set(RADRAY_RENDER_RUNTIME_BINDING_TEST_ENABLED OFF)
if (RADRAY_ENABLE_DXC)
    if (RADRAY_ENABLE_D3D12)
        set(RADRAY_RENDER_RUNTIME_BINDING_TEST_ENABLED ON)
    endif()
    if (RADRAY_ENABLE_VULKAN AND RADRAY_ENABLE_SPVCROSS)
        set(RADRAY_RENDER_RUNTIME_BINDING_TEST_ENABLED ON)
    endif()
endif()
if (RADRAY_RENDER_RUNTIME_BINDING_TEST_ENABLED)
    radray_add_test(
        test_compute_binding
        SOURCES
            render_test_framework.cpp
            test_compute_binding.cpp
        LINK_LIBS
            radraycore
            radrayrender)
endif()
if (RADRAY_BUILD_WINDOW)
    radray_add_test(test_rhi_swapchain SOURCES test_rhi_swapchain.cpp LINK_LIBS radraycore radrayrender radraywindow)
endif()
