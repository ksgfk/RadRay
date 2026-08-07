radray_add_test(
    test_radray_shader_compiler_client
    SOURCES test_radray_shader_compiler_client.cpp
    LINK_LIBS radrayshadercompiler)
target_link_libraries(test_radray_shader_compiler_client PRIVATE RadRayDXC::Headers)
radray_deploy_dxc_runtime(test_radray_shader_compiler_client)
if (MSVC)
    target_link_options(
        test_radray_shader_compiler_client
        PRIVATE
            "$<$<CONFIG:Debug>:-map:${RADRAY_BUILD_PATH}/Debug/test_radray_shader_compiler_client.map>"
            "$<$<CONFIG:Release>:-map:${RADRAY_BUILD_PATH}/Release/test_radray_shader_compiler_client.map>"
            "$<$<CONFIG:RelWithDebInfo>:-map:${RADRAY_BUILD_PATH}/RelWithDebInfo/test_radray_shader_compiler_client.map>"
            "$<$<CONFIG:MinSizeRel>:-map:${RADRAY_BUILD_PATH}/MinSizeRel/test_radray_shader_compiler_client.map>")
endif()
radray_add_test(
    test_radray_dxc_metadata
    SOURCES test_radray_dxc_metadata.cpp
    LINK_LIBS radrayshadercompiler)
radray_deploy_dxc_runtime(test_radray_dxc_metadata)
target_compile_definitions(test_radray_dxc_metadata PRIVATE RADRAY_PROJECT_DIR="${CMAKE_SOURCE_DIR}")
radray_add_test(
    test_shaderlib_passes
    SOURCES test_shaderlib_passes.cpp
    LINK_LIBS radrayshadercompiler)
radray_deploy_dxc_runtime(test_shaderlib_passes)
target_compile_definitions(test_shaderlib_passes PRIVATE RADRAY_PROJECT_DIR="${CMAKE_SOURCE_DIR}")
