radray_add_test(test_render_pipeline SOURCES test_render_pipeline.cpp test_runtime_layering.cpp LINK_LIBS radrayruntime)
target_include_directories(test_render_pipeline PRIVATE "${CMAKE_SOURCE_DIR}/modules/render/tests" "${CMAKE_SOURCE_DIR}/modules/runtime/src")
target_compile_definitions(test_render_pipeline PRIVATE RADRAY_PROJECT_DIR="${CMAKE_SOURCE_DIR}")
radray_add_test(test_asset_slot SOURCES test_asset_slot.cpp LINK_LIBS radrayruntime)
radray_add_test(test_asset_database SOURCES test_asset_database.cpp LINK_LIBS radrayruntime)
radray_add_test(test_material SOURCES test_material.cpp LINK_LIBS radrayruntime)
target_include_directories(test_material PRIVATE
    "${CMAKE_SOURCE_DIR}/modules/render/tests")
target_compile_definitions(test_material PRIVATE RADRAY_PROJECT_DIR="${CMAKE_SOURCE_DIR}")
radray_add_test(test_mesh_draw SOURCES test_mesh_draw.cpp LINK_LIBS radrayruntime)
target_include_directories(test_mesh_draw PRIVATE
    "${CMAKE_SOURCE_DIR}/modules/runtime/src"
    "${CMAKE_SOURCE_DIR}/modules/render/tests")
target_compile_definitions(test_mesh_draw PRIVATE RADRAY_PROJECT_DIR="${CMAKE_SOURCE_DIR}")

if (RADRAY_ENABLE_SHADER_JIT)
    radray_add_test(test_forward_pipeline SOURCES test_forward_pipeline.cpp test_forward_data.cpp LINK_LIBS radrayruntime)
    target_include_directories(test_forward_pipeline PRIVATE "${CMAKE_SOURCE_DIR}/modules/runtime/src" "${CMAKE_SOURCE_DIR}/modules/render/tests")
    target_compile_definitions(test_forward_pipeline PRIVATE RADRAY_PROJECT_DIR="${CMAKE_SOURCE_DIR}")
    radray_add_test(test_runtime_shader_jit SOURCES test_runtime_shader_jit.cpp LINK_LIBS radrayruntime)
    target_include_directories(test_runtime_shader_jit PRIVATE
        "${CMAKE_SOURCE_DIR}/modules/render/tests")
    target_link_libraries(test_runtime_shader_jit PRIVATE RadRayDXC::Headers)
    target_compile_definitions(test_runtime_shader_jit PRIVATE RADRAY_PROJECT_DIR="${CMAKE_SOURCE_DIR}")
endif()
