# radray_add_test(test_render_graph_valid SOURCES test_render_graph_valid.cpp LINK_LIBS radraycore radrayrender radrayruntime)
# radray_add_test(test_application SOURCES test_application.cpp LINK_LIBS radraycore radrayrender radraywindow radrayruntime)
radray_add_test(test_asset_manager SOURCES test_asset_manager.cpp LINK_LIBS radraycore radrayrender radrayruntime)
radray_add_test(test_render_pipeline SOURCES test_render_pipeline.cpp LINK_LIBS radraycore radrayrender radrayruntime)
if (RADRAY_ENABLE_DXC AND RADRAY_ENABLE_SPVCROSS)
    radray_add_test(test_material_parameter_layout SOURCES test_material_parameter_layout.cpp LINK_LIBS radraycore radrayrender radrayruntime)
endif()
if (RADRAY_ENABLE_DXC)
    radray_add_test(test_material_render_proxy SOURCES test_material_render_proxy.cpp LINK_LIBS radraycore radrayrender radrayruntime)
endif()

add_test(
    NAME RuntimeCoroutineBoundaryTest
    COMMAND ${CMAKE_COMMAND}
        -DRADRAY_RUNTIME_DIR=${CMAKE_SOURCE_DIR}/modules/runtime
        -P ${CMAKE_CURRENT_SOURCE_DIR}/check_runtime_coroutine_boundary.cmake)
