if (NOT RADRAY_BUILD_SHADER_COMPILER)
    message(FATAL_ERROR "RADRAY_BUILD_SHADER_TOOLS requires RADRAY_BUILD_SHADER_COMPILER")
endif()

add_subdirectory(shader_compile)
if (RADRAY_ENABLE_IMGUI)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if (Python3_Interpreter_FOUND)
        add_custom_target(radray_imgui_shaders_check
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_imgui_shaders.py"
                    --compiler "$<TARGET_FILE:radray_shader_compile>" --check
            DEPENDS radray_shader_compile VERBATIM)
        add_custom_target(radray_imgui_shaders_regenerate
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_imgui_shaders.py"
                    --compiler "$<TARGET_FILE:radray_shader_compile>"
            DEPENDS radray_shader_compile VERBATIM)
    endif()
endif()
