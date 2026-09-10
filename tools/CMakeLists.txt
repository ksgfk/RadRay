if (NOT RADRAY_BUILD_SHADER_COMPILER)
    message(FATAL_ERROR "RADRAY_BUILD_SHADER_TOOLS requires RADRAY_BUILD_SHADER_COMPILER")
endif()

add_subdirectory(shader_compile)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if (Python3_Interpreter_FOUND)
        add_custom_target(radray_builtin_shaders_check
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_builtin_shaders.py"
                    --compiler "$<TARGET_FILE:radray_shader_compile>" --check
            DEPENDS radray_shader_compile VERBATIM)
        add_custom_target(radray_builtin_shaders_regenerate
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_builtin_shaders.py"
                    --compiler "$<TARGET_FILE:radray_shader_compile>"
            DEPENDS radray_shader_compile VERBATIM)

        # The POD generator never calls DXC, so the layout owner is compiled here and its metadata
        # blobs are handed over. Neither target is part of the default build: the header is checked
        # in and only has to be regenerated when the Forward cbuffer ABI or the wire schema moves.
        set(RADRAY_FORWARD_CBUFFER_HEADER
            "${CMAKE_SOURCE_DIR}/modules/runtime/include/radray/runtime/forward_pipeline/gen_forward_cbuffers.h")
        set(RADRAY_LAYOUT_OWNER_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/forward_layout_owner")
        foreach (mode check regenerate)
            set(RADRAY_FORWARD_CBUFFER_ARGS "")
            if (mode STREQUAL "check")
                set(RADRAY_FORWARD_CBUFFER_ARGS --check)
            endif()
            add_custom_target(radray_forward_cbuffers_${mode}
                COMMAND "$<TARGET_FILE:radray_shader_compile>"
                        --shader-root "${CMAKE_SOURCE_DIR}/shaderlib"
                        --include-path "${CMAKE_SOURCE_DIR}/shaderlib"
                        --source pipelines/forward/layout_owner.hlsl
                        --target all --output "${RADRAY_LAYOUT_OWNER_PREFIX}"
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_forward_cbuffers.py"
                        "${RADRAY_LAYOUT_OWNER_PREFIX}.dxil.bin" "${RADRAY_LAYOUT_OWNER_PREFIX}.spirv.bin"
                        --output "${RADRAY_FORWARD_CBUFFER_HEADER}" ${RADRAY_FORWARD_CBUFFER_ARGS}
                DEPENDS radray_shader_compile VERBATIM)
        endforeach()
    endif()
