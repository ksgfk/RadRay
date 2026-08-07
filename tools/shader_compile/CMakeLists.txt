add_executable(radray_shader_compile shader_compile.cpp)
target_link_libraries(radray_shader_compile PRIVATE radrayshadercompiler)
radray_default_compile_flags(radray_shader_compile)
radray_optimize_flags_binary(radray_shader_compile)
radray_set_build_path(radray_shader_compile)

if (MSVC)
    target_link_options(
        radray_shader_compile
        PRIVATE
            "$<$<CONFIG:Debug>:-map:${RADRAY_BUILD_PATH}/Debug/radray_shader_compile.map>"
            "$<$<CONFIG:Release>:-map:${RADRAY_BUILD_PATH}/Release/radray_shader_compile.map>"
            "$<$<CONFIG:RelWithDebInfo>:-map:${RADRAY_BUILD_PATH}/RelWithDebInfo/radray_shader_compile.map>"
            "$<$<CONFIG:MinSizeRel>:-map:${RADRAY_BUILD_PATH}/MinSizeRel/radray_shader_compile.map>")
endif()

radray_deploy_dxc_runtime(radray_shader_compile)
