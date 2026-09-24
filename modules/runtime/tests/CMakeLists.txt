# 桌面 / 长 GPU 用例需要串行和更长超时。gtest_discover_tests 的 PROPERTIES 作用于一次发现的全部用例，
# 所以同一可执行文件做两次发现：默认用例，以及下面这些 suite。两次 POST_BUILD 顺序执行，
# 发现 JSON 按 target 哈希，不会并行争用。
set(radray_runtime_serial_suites
    "*SceneGpu*"
    "SceneDelivery.*"
    "SceneDeliveryDeathTest.*"
    "SceneDeliveryRunner.*"
    "MultiWorldSceneRunner.*"
    "SceneDraw.*"
    "GpuSceneLifetime.*"
    "RuntimeMultiWindow.*"
    "RuntimeVulkanSwapChain*")
list(JOIN radray_runtime_serial_suites ":" radray_runtime_serial_filter)

set(radray_runtime_test_sources
    test_actor_component_retirement.cpp
    test_application.cpp
    test_application_systems.cpp
    test_asset_database.cpp
    test_asset_slot.cpp
    test_component_rtti.cpp
    test_frame_scenarios.cpp
    test_frame_upload.cpp
    test_gpu_system.cpp
    test_lifecycle_scale.cpp
    test_scene_apply_changes.cpp
    test_scene_assets.cpp
    test_scene_delivery.cpp
    test_scene_delivery_state.cpp
    test_scene_draw.cpp
    test_scene_gpu.cpp
    test_scene_sync.cpp
    test_scene_updates.cpp
    test_scene_views.cpp
    test_spot_light.cpp
    test_static_mesh_scene.cpp
    test_window_operations.cpp
    test_world_lifecycle.cpp
    test_world_scenes.cpp
    test_multi_window.cpp
    scene_sync_workload.cpp
    "${CMAKE_SOURCE_DIR}/examples/scene_sync/scene_draw.cpp")
if (RADRAY_ENABLE_SHADER_JIT)
    list(APPEND radray_runtime_test_sources
        test_gpu_scene_lifetime.cpp
        test_runtime_shader_jit.cpp)
endif()

radray_add_test(test_radray_runtime
    SOURCES ${radray_runtime_test_sources}
    LINK_LIBS radrayruntime
    DISCOVER_ARGS TEST_FILTER "-${radray_runtime_serial_filter}")
target_include_directories(test_radray_runtime PRIVATE
    "${CMAKE_SOURCE_DIR}/modules/render/tests"
    "${CMAKE_SOURCE_DIR}/examples/scene_sync")
target_compile_definitions(test_radray_runtime PRIVATE
    RADRAY_SCENE_EXAMPLE_DIR="${CMAKE_SOURCE_DIR}/examples/scene_sync"
    RADRAY_SHADERLIB_DIR="${CMAKE_SOURCE_DIR}/shaderlib")
if (RADRAY_ENABLE_SHADER_JIT)
    target_include_directories(test_radray_runtime PRIVATE "${CMAKE_SOURCE_DIR}/modules/runtime/src")
    target_link_libraries(test_radray_runtime PRIVATE RadRayDXC::Headers)
    target_compile_definitions(test_radray_runtime PRIVATE
        RADRAY_SCENE_JIT_AVAILABLE
        RADRAY_PROJECT_DIR="${CMAKE_SOURCE_DIR}")
endif()
gtest_discover_tests(test_radray_runtime
    TEST_FILTER "${radray_runtime_serial_filter}"
    PROPERTIES RUN_SERIAL TRUE TIMEOUT 90)

add_custom_target(radray_runtime_tests DEPENDS test_radray_runtime)
