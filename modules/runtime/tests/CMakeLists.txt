# Runtime foundation tests; graph/Forward/UI tests were removed with the old renderer.
foreach(topic actor_component_retirement application asset_database asset_slot component_rtti frame_upload gpu_system scene_delivery_state scene_assets scene_updates static_mesh_scene spot_light window_operations world_scenes world_lifecycle lifecycle_performance)
    radray_add_test(test_${topic} SOURCES test_${topic}.cpp LINK_LIBS radrayruntime)
endforeach()
radray_add_test(test_scene_sync_performance SOURCES test_scene_sync_performance.cpp LINK_LIBS radrayruntime
    DISCOVER_ARGS PROPERTIES RUN_SERIAL TRUE TIMEOUT 1800)
target_include_directories(test_application PRIVATE "${CMAKE_SOURCE_DIR}/modules/render/tests")
target_include_directories(test_gpu_system PRIVATE "${CMAKE_SOURCE_DIR}/modules/render/tests")

radray_add_test(test_scene_delivery SOURCES test_scene_delivery.cpp LINK_LIBS radrayruntime
    DISCOVER_ARGS PROPERTIES RUN_SERIAL TRUE TIMEOUT 90)
target_include_directories(test_scene_delivery PRIVATE "${CMAKE_SOURCE_DIR}/modules/render/tests")

radray_add_test(test_multi_window SOURCES test_multi_window.cpp LINK_LIBS radrayruntime
    DISCOVER_ARGS PROPERTIES RUN_SERIAL TRUE TIMEOUT 90)
target_include_directories(test_multi_window PRIVATE "${CMAKE_SOURCE_DIR}/modules/render/tests")

if (RADRAY_ENABLE_SHADER_JIT)
    radray_add_test(test_gpu_scene_lifetime SOURCES test_gpu_scene_lifetime.cpp LINK_LIBS radrayruntime
        DISCOVER_ARGS PROPERTIES RUN_SERIAL TRUE TIMEOUT 90)
    target_include_directories(test_gpu_scene_lifetime PRIVATE "${CMAKE_SOURCE_DIR}/modules/render/tests")
    radray_add_test(test_runtime_shader_jit SOURCES test_runtime_shader_jit.cpp LINK_LIBS radrayruntime)
    target_include_directories(test_runtime_shader_jit PRIVATE
        "${CMAKE_SOURCE_DIR}/modules/render/tests" "${CMAKE_SOURCE_DIR}/modules/runtime/src")
    target_link_libraries(test_runtime_shader_jit PRIVATE RadRayDXC::Headers)
    target_compile_definitions(test_runtime_shader_jit PRIVATE RADRAY_PROJECT_DIR="${CMAKE_SOURCE_DIR}")
endif()

get_property(radray_runtime_test_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
add_custom_target(radray_runtime_tests)
foreach(radray_runtime_test_target IN LISTS radray_runtime_test_targets)
    get_target_property(radray_runtime_test_type ${radray_runtime_test_target} TYPE)
    if(radray_runtime_test_type STREQUAL "EXECUTABLE" AND radray_runtime_test_target MATCHES "^test_")
        add_dependencies(radray_runtime_tests ${radray_runtime_test_target})
    endif()
endforeach()
