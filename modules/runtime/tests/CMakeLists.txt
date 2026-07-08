radray_add_test(test_asset_manager SOURCES test_asset_manager.cpp LINK_LIBS radraycore radrayrender radrayruntime)
radray_add_test(test_deferred_render_delete_queue SOURCES test_deferred_render_delete_queue.cpp LINK_LIBS radraycore radrayrender radrayruntime)
radray_add_test(test_light_component SOURCES test_light_component.cpp LINK_LIBS radraycore radrayrender radrayruntime)
radray_add_test(test_static_mesh_component SOURCES test_static_mesh_component.cpp LINK_LIBS radraycore radrayrender radrayruntime)
radray_add_test(test_shader_asset SOURCES test_shader_asset.cpp LINK_LIBS radraycore radrayrender radrayruntime)
radray_add_test(test_material_asset SOURCES test_material_asset.cpp LINK_LIBS radraycore radrayrender radrayruntime)
radray_add_test(test_material_property_block SOURCES test_material_property_block.cpp LINK_LIBS radraycore radrayrender radrayruntime)
radray_add_test(test_render_queue SOURCES test_render_queue.cpp LINK_LIBS radraycore radrayrender radrayruntime)

# 设备级集成测试: material -> variant -> parameter table 全链路。复用 render 测试框架
# (仅依赖 core+render), 需要 DXC; Vulkan 后端还需 SPIR-V Cross 反射。
if (RADRAY_ENABLE_DXC AND (RADRAY_ENABLE_D3D12 OR (RADRAY_ENABLE_VULKAN AND RADRAY_ENABLE_SPVCROSS)))
    radray_add_test(test_material_variant_integration
        SOURCES
            test_material_variant_integration.cpp
            ${CMAKE_SOURCE_DIR}/modules/render/tests/render_test_framework.cpp
        LINK_LIBS radraycore radrayrender radrayruntime
        NO_DISCOVER)
    target_include_directories(test_material_variant_integration PRIVATE ${CMAKE_SOURCE_DIR}/modules/render/tests)
    radray_add_gtest_case(MaterialVariantIntegrationTest TARGET test_material_variant_integration FILTER RenderBackends/MaterialVariantIntegrationTest.*)

    radray_add_test(test_pipeline_pass_resolve
        SOURCES
            test_pipeline_pass_resolve.cpp
            ${CMAKE_SOURCE_DIR}/modules/render/tests/render_test_framework.cpp
        LINK_LIBS radraycore radrayrender radrayruntime
        NO_DISCOVER)
    target_include_directories(test_pipeline_pass_resolve PRIVATE ${CMAKE_SOURCE_DIR}/modules/render/tests)
    radray_add_gtest_case(PipelinePassResolveTest TARGET test_pipeline_pass_resolve FILTER RenderBackends/PipelinePassResolveTest.*)

    # 逐物体绘制闭环: DrawList -> MeshPassExecutor -> 离屏 RT 实际绘制 + 像素回读验证。
    radray_add_test(test_mesh_pass_executor
        SOURCES
            test_mesh_pass_executor.cpp
            ${CMAKE_SOURCE_DIR}/modules/render/tests/render_test_framework.cpp
        LINK_LIBS radraycore radrayrender radrayruntime
        NO_DISCOVER)
    target_include_directories(test_mesh_pass_executor PRIVATE ${CMAKE_SOURCE_DIR}/modules/render/tests)
    radray_add_gtest_case(MeshPassExecutorTest TARGET test_mesh_pass_executor FILTER RenderBackends/MeshPassExecutorTest.*)

    # TextureAsset view 缓存: 默认 SRV 直返, 非默认子 view 按 descriptor 去重。
    radray_add_test(test_texture_view_cache
        SOURCES
            test_texture_view_cache.cpp
            ${CMAKE_SOURCE_DIR}/modules/render/tests/render_test_framework.cpp
        LINK_LIBS radraycore radrayrender radrayruntime
        NO_DISCOVER)
    target_include_directories(test_texture_view_cache PRIVATE ${CMAKE_SOURCE_DIR}/modules/render/tests)
    radray_add_gtest_case(TextureViewCacheTest TARGET test_texture_view_cache FILTER RenderBackends/TextureViewCacheTest.*)

    # 材质常量按【字段名】打进真实 cbuffer (MaterialConstantBinder 反射打包): 字段名设色 -> 像素回读。
    radray_add_test(test_material_cbuffer_field
        SOURCES
            test_material_cbuffer_field.cpp
            ${CMAKE_SOURCE_DIR}/modules/render/tests/render_test_framework.cpp
        LINK_LIBS radraycore radrayrender radrayruntime
        NO_DISCOVER)
    target_include_directories(test_material_cbuffer_field PRIVATE ${CMAKE_SOURCE_DIR}/modules/render/tests)
    radray_add_gtest_case(MaterialCBufferFieldTest TARGET test_material_cbuffer_field FILTER RenderBackends/MaterialCBufferFieldTest.*)
endif()
