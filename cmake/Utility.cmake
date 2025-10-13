# Guard against multiple inclusion
if (DEFINED RADRAY_CMAKE_UTILITY_INCLUDED)
    return()
endif()
set(RADRAY_CMAKE_UTILITY_INCLUDED TRUE)

# For compiler flag feature detection
include(CheckCXXCompilerFlag)
include(CheckLinkerFlag)

function(radray_set_build_path TARGET)
    set_target_properties(${TARGET} PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY ${RADRAY_BUILD_PATH}
        LIBRARY_OUTPUT_DIRECTORY ${RADRAY_BUILD_PATH}
        RUNTIME_OUTPUT_DIRECTORY ${RADRAY_BUILD_PATH}
        PDB_OUTPUT_DIRECTORY ${RADRAY_BUILD_PATH})
endfunction()

function(radray_example_files TARGET_NAME)
    if (ARGC LESS 2)
        message(FATAL_ERROR "radray_example_files: 需要 <TARGET_NAME> 和至少一个 <SRC>")
    endif()
    if (NOT TARGET ${TARGET_NAME})
        message(FATAL_ERROR "radray_example_files: 目标 ${TARGET_NAME} 未定义")
    endif()

    set(_target_name "${ARGV0}")
    list(REMOVE_AT ARGV 0)
    set(_sources ${ARGV})
    set(_dest_root "${CMAKE_SOURCE_DIR}/assets/${_target_name}")

    set(_POST_BUILD_CMDS
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_dest_root}"
    )
    foreach(_src IN LISTS _sources)
        if (NOT IS_ABSOLUTE "${_src}")
            set(_abs_src "${CMAKE_CURRENT_SOURCE_DIR}/${_src}")
        else()
            set(_abs_src "${_src}")
        endif()
        if (NOT EXISTS "${_abs_src}")
            message(FATAL_ERROR "radray_example_files: 源不存在: ${_abs_src}")
        endif()

        if (IS_DIRECTORY "${_abs_src}")
            file(RELATIVE_PATH _rel_dir "${CMAKE_CURRENT_SOURCE_DIR}" "${_abs_src}")
            if (_rel_dir STREQUAL "")
                set(_rel_dir ".")
            endif()
            list(APPEND _POST_BUILD_CMDS
                COMMAND ${CMAKE_COMMAND} -E make_directory "${_dest_root}/${_rel_dir}"
                COMMAND ${CMAKE_COMMAND} -E copy_directory "${_abs_src}" "${_dest_root}/${_rel_dir}"
            )
        else()
            file(RELATIVE_PATH _rel_file "${CMAKE_CURRENT_SOURCE_DIR}" "${_abs_src}")
            get_filename_component(_rel_dir "${_rel_file}" DIRECTORY)
            if (_rel_dir STREQUAL "")
                set(_rel_dir ".")
            endif()
            list(APPEND _POST_BUILD_CMDS
                COMMAND ${CMAKE_COMMAND} -E make_directory "${_dest_root}/${_rel_dir}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_abs_src}" "${_dest_root}/${_rel_dir}"
            )
        endif()
    endforeach()

    add_custom_command(TARGET ${_target_name} POST_BUILD
        ${_POST_BUILD_CMDS}
        COMMENT "Copy example files to ${_dest_root}")
endfunction()

# radray_compile_flag_auto_simd(<target>)
# 目前：为 target 启用 AVX2。区分 MSVC/clang-cl、GCC、Clang。
function(radray_compile_flag_auto_simd target)
    if (NOT TARGET ${target})
        message(FATAL_ERROR "radray_compile_flag_auto_simd: '${target}' is not a valid target")
    endif()

    # 仅在 x64 上尝试设置 SIMD 相关编译标志（不考虑 32 位）
    if (NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
        message(STATUS "Architecture '${CMAKE_SYSTEM_PROCESSOR}' is not x86/x64; skipping SIMD flags for target ${target}")
        # 记录跳过结果，避免后续 target 重复判断（仅在本次配置会话内）
        get_property(_simd_cached GLOBAL PROPERTY RADRAY_DETECTED_SIMD_FLAG)
        if (NOT _simd_cached)
            set_property(GLOBAL PROPERTY RADRAY_DETECTED_SIMD_FLAG "SKIP")
        endif()
        return()
    endif()

    # 若已有缓存结果，直接复用
    get_property(_simd_cached GLOBAL PROPERTY RADRAY_DETECTED_SIMD_FLAG)
    if (_simd_cached)
        if (NOT _simd_cached STREQUAL "NONE" AND NOT _simd_cached STREQUAL "SKIP")
            target_compile_options(${target} PRIVATE ${_simd_cached})
            message(STATUS "Applied cached SIMD flag '${_simd_cached}' to target ${target}.")
        else()
            message(STATUS "SIMD flag detection previously found none/skip; skipping for target ${target}.")
        endif()
        return()
    endif()

    # 判断是否为 MSVC 前端（包含 MSVC 与 clang-cl）
    set(_is_msvc_frontend FALSE)
    if (MSVC OR
        CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC" OR
        CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC" OR
        CMAKE_C_SIMULATE_ID STREQUAL "MSVC")
        set(_is_msvc_frontend TRUE)
    endif()

    set(_candidate_flags)
    if (_is_msvc_frontend)
        # MSVC/clang-cl 使用 /arch:*
        list(APPEND _candidate_flags "/arch:AVX2" "/arch:AVX" "/arch:SSE2" "/arch:SSE")
    else()
        # GNU/Clang 前端使用 -m*
        list(APPEND _candidate_flags "-mavx2" "-mavx" "-msse4.2" "-msse2" "-msse")
    endif()

    set(_applied_flag "")
    foreach(_flag IN LISTS _candidate_flags)
        # 将标志转为变量名友好格式
        string(REGEX REPLACE "[^A-Za-z0-9_]" "_" _flag_key "${_flag}")
        set(_var_name "HAVE_FLAG_${_flag_key}")
        unset(${_var_name} CACHE)
        check_cxx_compiler_flag("${_flag}" ${_var_name})
        if (${_var_name})
            target_compile_options(${target} PRIVATE ${_flag})
            set(_applied_flag "${_flag}")
            break()
        endif()
    endforeach()

    if (_applied_flag STREQUAL "")
        set_property(GLOBAL PROPERTY RADRAY_DETECTED_SIMD_FLAG "NONE")
        message(WARNING "No supported SIMD flag (fallback AVX2→AVX→SSE*) found for target ${target} with compiler '${CMAKE_CXX_COMPILER_ID}'.")
    else()
        # 若为 GNU/Clang 且已至少为 AVX 级别，额外尝试开启 FMA（-mfma）
        set(_final_flags "${_applied_flag}")
        if (NOT _is_msvc_frontend AND ("${_applied_flag}" STREQUAL "-mavx2" OR "${_applied_flag}" STREQUAL "-mavx"))
            set(_fma_var "HAVE_FLAG__mfma")
            unset(${_fma_var} CACHE)
            check_cxx_compiler_flag("-mfma" ${_fma_var})
            if (${_fma_var})
                target_compile_options(${target} PRIVATE -mfma)
                list(APPEND _final_flags "-mfma")
                message(STATUS "Also enabled FMA with '-mfma' for target ${target}.")
            endif()
        endif()

        # 缓存组合后的标志列表（本次配置会话内）
        list(JOIN _final_flags ";" _final_flags_joined)
        set_property(GLOBAL PROPERTY RADRAY_DETECTED_SIMD_FLAG "${_final_flags_joined}")
        message(STATUS "Applied SIMD flag(s) '${_final_flags_joined}' to target ${target}.")
    endif()
endfunction()

# radray_link_flag_lto(<target>)
# 为 target 添加链接时优化（LTO）相关的链接器标志，仅在 Release 配置下生效；按不同编译器/链接器进行区分并检测可用性。
# 注意：部分工具链需要在编译阶段也加对应的编译标志（例如 MSVC /GL，GCC/Clang -flto），本函数仅负责链接阶段。
function(radray_link_flag_lto target)
    if (NOT TARGET ${target})
        message(FATAL_ERROR "radray_link_flag_lto: '${target}' is not a valid target")
    endif()

    # 若已有缓存结果，直接复用（仅在当前配置会话内）
    get_property(_lto_link_cached GLOBAL PROPERTY RADRAY_DETECTED_LTO_LINK_FLAG)
    if (_lto_link_cached)
        if (NOT _lto_link_cached STREQUAL "NONE")
            target_link_options(${target} PRIVATE $<$<CONFIG:Release>:${_lto_link_cached}>)
            message(STATUS "Applied cached LTO linker flag '${_lto_link_cached}' to target ${target} (Release only).")
        else()
            message(STATUS "LTO linker flag detection previously found none; skipping for target ${target}.")
        endif()
        return()
    endif()

    # 判断是否为 MSVC 前端（包含 MSVC 与 clang-cl）
    set(_is_msvc_frontend FALSE)
    if (MSVC OR
        CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC" OR
        CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC" OR
        CMAKE_C_SIMULATE_ID STREQUAL "MSVC")
        set(_is_msvc_frontend TRUE)
    endif()

    # 区分 clang-cl 与 纯 MSVC
    set(_is_clang_cl FALSE)
    if (_is_msvc_frontend AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        set(_is_clang_cl TRUE)
    endif()

    # 仅对纯 MSVC 传递链接期 LTO 标志；GCC/Clang/clang-cl 无需显式链接标志
    set(_candidate_link_flags)
    if (_is_msvc_frontend AND NOT _is_clang_cl)
        # 纯 MSVC: /LTCG 及其可选增量模式
        list(APPEND _candidate_link_flags "/LTCG" "/LTCG:incremental")
    else()
        set_property(GLOBAL PROPERTY RADRAY_DETECTED_LTO_LINK_FLAG "NONE")
        message(STATUS "Toolchain '${CMAKE_CXX_COMPILER_ID}' does not require explicit LTO link flags; skipping for target ${target}.")
        return()
    endif()

    set(_applied_link_flag "")
    foreach(_lflag IN LISTS _candidate_link_flags)
        string(REGEX REPLACE "[^A-Za-z0-9_]" "_" _lflag_key "${_lflag}")
        set(_var_name "HAVE_LINK_FLAG_${_lflag_key}")
        unset(${_var_name} CACHE)
        check_linker_flag(CXX "${_lflag}" ${_var_name})
        if (${_var_name})
            # 仅在 Release 配置生效
            target_link_options(${target} PRIVATE $<$<CONFIG:Release>:${_lflag}>)
            set(_applied_link_flag "${_lflag}")
            break()
        endif()
    endforeach()

    if (_applied_link_flag STREQUAL "")
        set_property(GLOBAL PROPERTY RADRAY_DETECTED_LTO_LINK_FLAG "NONE")
        message(WARNING "No supported LTO linker flag found for target ${target} (compiler='${CMAKE_CXX_COMPILER_ID}').")
    else()
        set_property(GLOBAL PROPERTY RADRAY_DETECTED_LTO_LINK_FLAG "${_applied_link_flag}")
        message(STATUS "Applied LTO linker flag '${_applied_link_flag}' to target ${target} (Release only).")
    endif()
endfunction()

# radray_compile_flag_lto(<target>)
# 为 target 添加编译时优化（LTO）相关的编译器标志，仅在 Release 配置下生效；按不同编译器进行区分并检测可用性。
function(radray_compile_flag_lto target)
    if (NOT TARGET ${target})
        message(FATAL_ERROR "radray_compile_flag_lto: '${target}' is not a valid target")
    endif()

    # 单配置生成器且非 Release 直接跳过
    if (DEFINED CMAKE_BUILD_TYPE AND NOT CMAKE_BUILD_TYPE STREQUAL "Release")
        message(STATUS "Skipping compile-time LTO for target ${target} because CMAKE_BUILD_TYPE='${CMAKE_BUILD_TYPE}' is not Release.")
        return()
    endif()

    # 缓存命中直接应用（仅在当前配置会话内）
    get_property(_lto_compile_cached GLOBAL PROPERTY RADRAY_DETECTED_LTO_COMPILE_FLAG)
    if (_lto_compile_cached)
        if (NOT _lto_compile_cached STREQUAL "NONE")
            target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:${_lto_compile_cached}>)
            message(STATUS "Applied cached LTO compile flag '${_lto_compile_cached}' to target ${target} (Release only).")
        else()
            message(STATUS "LTO compile flag detection previously found none; skipping for target ${target}.")
        endif()
        return()
    endif()

    # 判断是否为 MSVC 前端（包含 MSVC 与 clang-cl）
    set(_is_msvc_frontend FALSE)
    if (MSVC OR
        CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC" OR
        CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC" OR
        CMAKE_C_SIMULATE_ID STREQUAL "MSVC")
        set(_is_msvc_frontend TRUE)
    endif()

    # 区分 clang-cl 与 纯 MSVC
    set(_is_clang_cl FALSE)
    if (_is_msvc_frontend AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        set(_is_clang_cl TRUE)
    endif()

    set(_candidate_compile_flags)
    if (_is_clang_cl)
        # clang-cl 优先 -flto=thin/-flto，必要时回退 /GL
        list(APPEND _candidate_compile_flags "-flto=thin" "-flto" "/GL")
    elseif (_is_msvc_frontend)
        # 纯 MSVC: /GL 开启 LTCG
        list(APPEND _candidate_compile_flags "/GL")
    else()
        if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            # Clang: 优先 ThinLTO
            list(APPEND _candidate_compile_flags "-flto=thin" "-flto")
        elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            list(APPEND _candidate_compile_flags "-flto" "-flto=auto")
        else()
            list(APPEND _candidate_compile_flags "-flto")
        endif()
    endif()

    set(_applied_compile_flag "")
    foreach(_cflag IN LISTS _candidate_compile_flags)
        string(REGEX REPLACE "[^A-Za-z0-9_]" "_" _cflag_key "${_cflag}")
        set(_var_name "HAVE_COMPILE_FLAG_${_cflag_key}")
        unset(${_var_name} CACHE)
        check_cxx_compiler_flag("${_cflag}" ${_var_name})
        if (${_var_name})
            target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:${_cflag}>)
            set(_applied_compile_flag "${_cflag}")
            break()
        endif()
    endforeach()

    if (_applied_compile_flag STREQUAL "")
        set_property(GLOBAL PROPERTY RADRAY_DETECTED_LTO_COMPILE_FLAG "NONE")
        message(WARNING "No supported LTO compile flag found for target ${target} (compiler='${CMAKE_CXX_COMPILER_ID}').")
    else()
        set_property(GLOBAL PROPERTY RADRAY_DETECTED_LTO_COMPILE_FLAG "${_applied_compile_flag}")
        message(STATUS "Applied LTO compile flag '${_applied_compile_flag}' to target ${target} (Release only).")
    endif()
endfunction()
