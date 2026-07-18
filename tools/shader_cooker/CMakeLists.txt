add_executable(radray_shader_cooker main.cpp)
target_link_libraries(radray_shader_cooker PRIVATE radrayshader)
radray_default_compile_flags(radray_shader_cooker)
radray_optimize_flags_binary(radray_shader_cooker)
radray_set_build_path(radray_shader_cooker)

set(_RADRAY_SHADER_COOKER_DEPLOY_COMMANDS
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:radray_shader_cooker>")
foreach(_dll IN LISTS RADRAY_DXC_RUNTIME_DLLS)
    list(APPEND _RADRAY_SHADER_COOKER_DEPLOY_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_dll}" "$<TARGET_FILE_DIR:radray_shader_cooker>")
endforeach()
add_custom_command(TARGET radray_shader_cooker POST_BUILD
    ${_RADRAY_SHADER_COOKER_DEPLOY_COMMANDS}
    COMMENT "Deploy DXC runtime for shader cooker")
unset(_RADRAY_SHADER_COOKER_DEPLOY_COMMANDS)
