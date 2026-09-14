if (NOT RADRAY_BUILD_SHADER_COMPILER)
    message(FATAL_ERROR "RADRAY_BUILD_SHADER_TOOLS requires RADRAY_BUILD_SHADER_COMPILER")
endif()

add_subdirectory(shader_compile)
