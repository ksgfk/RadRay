# radray_add_test(test_render_graph_valid SOURCES test_render_graph_valid.cpp LINK_LIBS radraycore radrayrender radrayruntime)
# radray_add_test(test_application SOURCES test_application.cpp LINK_LIBS radraycore radrayrender radraywindow radrayruntime)
radray_add_test(test_asset_manager SOURCES test_asset_manager.cpp LINK_LIBS radraycore radrayrender radrayruntime)
radray_add_test(test_deferred_render_delete_queue SOURCES test_deferred_render_delete_queue.cpp LINK_LIBS radraycore radrayrender radrayruntime)
radray_add_test(test_pso_cache SOURCES test_pso_cache.cpp LINK_LIBS radraycore radrayrender radrayruntime)
radray_add_test(test_shader_variant SOURCES test_shader_variant.cpp LINK_LIBS radraycore radrayruntime)
radray_add_test(test_srp_vocab SOURCES test_srp_vocab.cpp LINK_LIBS radraycore radrayrender radrayruntime)
radray_add_test(test_srp_data_layer SOURCES test_srp_data_layer.cpp LINK_LIBS radraycore radrayrender radrayruntime)
radray_add_test(test_srp_machine SOURCES test_srp_machine.cpp LINK_LIBS radraycore radrayrender radrayruntime)
radray_add_test(test_srp_orchestration SOURCES test_srp_orchestration.cpp LINK_LIBS radraycore radrayrender radrayruntime)
add_test(
    NAME RuntimeCoroutineBoundaryTest
    COMMAND ${CMAKE_COMMAND}
        -DRADRAY_RUNTIME_DIR=${CMAKE_SOURCE_DIR}/modules/runtime
        -P ${CMAKE_CURRENT_SOURCE_DIR}/check_runtime_coroutine_boundary.cmake)
