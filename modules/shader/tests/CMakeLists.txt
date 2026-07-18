radray_add_test(test_shader_binary
    SOURCES test_shader_binary.cpp
    LINK_LIBS radraycore radrayshader)
radray_add_test(test_reflection_serialize
    SOURCES test_reflection_serialize.cpp
    LINK_LIBS radraycore radrayshader)

if (RADRAY_ENABLE_DXC AND RADRAY_ENABLE_SPIRV_CROSS)
    radray_add_test(test_spvc_msl
        SOURCES test_spvc_msl.cpp
        LINK_LIBS radraycore radrayshader)
endif()

if (TARGET radray_shader_cooker)
    radray_add_test(test_shader_cooker
        SOURCES test_shader_cooker.cpp
        LINK_LIBS radraycore radrayshader)
    add_dependencies(test_shader_cooker radray_shader_cooker)
    target_compile_definitions(test_shader_cooker PRIVATE
        RADRAY_PROJECT_DIR="${CMAKE_SOURCE_DIR}"
        RADRAY_SHADER_COOKER_PATH="$<TARGET_FILE:radray_shader_cooker>")
endif()
