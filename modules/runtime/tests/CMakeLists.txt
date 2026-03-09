set(RADRAY_RUNTIME_DEVICE_TEST_ENABLED OFF)
if (RADRAY_ENABLE_D3D12)
    set(RADRAY_RUNTIME_DEVICE_TEST_ENABLED ON)
endif()
if (RADRAY_ENABLE_VULKAN)
    set(RADRAY_RUNTIME_DEVICE_TEST_ENABLED ON)
endif()

radray_add_test(
    test_runtime_snapshot_builder
    SOURCES
        test_frame_snapshot_builder.cpp
    LINK_LIBS
        radraycore
        radrayruntime)

if (RADRAY_RUNTIME_DEVICE_TEST_ENABLED)
    radray_add_test(
        test_runtime_render_asset_registry
        SOURCES
            runtime_test_framework.cpp
            test_render_asset_registry.cpp
        LINK_LIBS
            radraycore
            radrayrender
            radrayruntime)
endif()

set(RADRAY_RUNTIME_SMOKE_TEST_ENABLED OFF)
if (RADRAY_ENABLE_DXC AND RADRAY_BUILD_WINDOW)
    if (RADRAY_ENABLE_D3D12)
        set(RADRAY_RUNTIME_SMOKE_TEST_ENABLED ON)
    endif()
    if (RADRAY_ENABLE_VULKAN AND RADRAY_ENABLE_SPVCROSS)
        set(RADRAY_RUNTIME_SMOKE_TEST_ENABLED ON)
    endif()
endif()

if (RADRAY_RUNTIME_SMOKE_TEST_ENABLED)
    radray_add_test(
        test_renderer_runtime
        SOURCES
            runtime_test_framework.cpp
            test_renderer_runtime.cpp
        LINK_LIBS
            radraycore
            radrayrender
            radrayruntime
            radraywindow)
endif()
