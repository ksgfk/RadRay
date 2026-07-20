radray_add_test(test_asset_manager SOURCES test_asset_manager.cpp LINK_LIBS radraycore radrayrender radrayruntime)
radray_add_test(test_deferred_render_delete_queue SOURCES test_deferred_render_delete_queue.cpp LINK_LIBS radraycore radrayrender radrayruntime)
radray_add_test(test_staging_buffer_pool SOURCES test_staging_buffer_pool.cpp LINK_LIBS radraycore radrayrender radrayruntime)
radray_add_test(test_gpu_resource SOURCES test_gpu_resource.cpp LINK_LIBS radraycore radrayrender radrayruntime)
radray_add_test(test_mapped_memory SOURCES test_mapped_memory.cpp LINK_LIBS radraycore radrayrender radrayruntime)
radray_add_test(test_material_asset SOURCES test_material_asset.cpp LINK_LIBS radraycore radrayrender radrayruntime)
radray_add_test(test_shader_binding_policy SOURCES test_shader_binding_policy.cpp LINK_LIBS radraycore radrayrender radrayruntime)
radray_add_test(test_shader_parameters SOURCES test_shader_parameters.cpp LINK_LIBS radraycore radrayrender radrayruntime)
radray_add_test(test_shader_artifact_resolver SOURCES test_shader_artifact_resolver.cpp LINK_LIBS radraycore radrayrender radrayruntime)

if (TARGET radray_shader_cooker AND RADRAY_ENABLE_DXC AND RADRAY_ENABLE_SPIRV_CROSS)
    radray_add_test(test_forward_material_cooker
        SOURCES test_forward_material_cooker.cpp
        LINK_LIBS radraycore radrayrender radrayruntime)
    add_dependencies(test_forward_material_cooker radray_shader_cooker)
    target_compile_definitions(test_forward_material_cooker PRIVATE
        RADRAY_PROJECT_DIR="${CMAKE_SOURCE_DIR}"
        RADRAY_SHADER_COOKER_PATH="$<TARGET_FILE:radray_shader_cooker>")
endif()

if (RADRAY_ENABLE_SHADER_JIT AND (RADRAY_ENABLE_D3D12 OR (RADRAY_ENABLE_VULKAN AND RADRAY_ENABLE_SPIRV_CROSS)))
    # TextureAsset view 缓存: 默认 SRV 直返, 非默认子 view 按 descriptor 去重。
    radray_add_test(test_texture_view_cache
        SOURCES
            test_texture_view_cache.cpp
            ${CMAKE_SOURCE_DIR}/modules/render/tests/render_test_framework.cpp
        LINK_LIBS radraycore radrayrender radrayruntime
        NO_DISCOVER)
    target_include_directories(test_texture_view_cache PRIVATE ${CMAKE_SOURCE_DIR}/modules/render/tests)
    radray_add_gtest_case(TextureViewCacheTest TARGET test_texture_view_cache FILTER RenderBackends/TextureViewCacheTest.*)

    # Upload page 子分配: buffer/texture 共享 page，验证非零 copy offset 与回读。
    radray_add_test(test_resource_uploader
        SOURCES
            test_resource_uploader.cpp
            ${CMAKE_SOURCE_DIR}/modules/render/tests/render_test_framework.cpp
        LINK_LIBS radraycore radrayrender radrayruntime
        NO_DISCOVER)
    target_include_directories(test_resource_uploader PRIVATE ${CMAKE_SOURCE_DIR}/modules/render/tests)
    radray_add_gtest_case(ResourceUploaderRuntimeTest TARGET test_resource_uploader FILTER RenderBackends/ResourceUploaderRuntimeTest.*)
endif()
