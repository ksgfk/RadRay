# Guard against multiple inclusion
if (DEFINED RADRAY_CMAKE_UTILITY_INCLUDED)
    return()
endif()
set(RADRAY_CMAKE_UTILITY_INCLUDED TRUE)

include(CheckCXXCompilerFlag)
include(CheckLinkerFlag)
include(CheckIPOSupported)

# 探测第一个可用的编译器标志并写入全局属性
function(radray_detect_first_compiler_flag GLOBAL_PROPERTY)
    set(_applied_flag "")
    foreach(_flag IN LISTS ARGN)
        string(REGEX REPLACE "[^A-Za-z0-9_]" "_" _flag_key "${_flag}")
        set(_var_name "HAVE_FLAG_${_flag_key}")
        unset(${_var_name} CACHE)
    check_cxx_compiler_flag("${_flag}" ${_var_name})
        if (${_var_name})
            set(_applied_flag "${_flag}")
            break()
        endif()
    endforeach()
    if (NOT _applied_flag STREQUAL "")
        set_property(GLOBAL PROPERTY ${GLOBAL_PROPERTY} "${_applied_flag}")
    endif()
endfunction()

# 探测第一个可用的链接器标志并写入全局属性
function(radray_detect_first_linker_flag GLOBAL_PROPERTY)
    set(_applied_flag "")
    foreach(_lflag IN LISTS ARGN)
        string(REGEX REPLACE "[^A-Za-z0-9_]" "_" _flag_key "${_lflag}")
        set(_var_name "HAVE_LINKER_FLAG_${_flag_key}")
        unset(${_var_name} CACHE)
        check_linker_flag(CXX "${_lflag}" ${_var_name})
        if (${_var_name})
            set(_applied_flag "${_lflag}")
            break()
        endif()
    endforeach()
    if (NOT _applied_flag STREQUAL "")
        set_property(GLOBAL PROPERTY ${GLOBAL_PROPERTY} "${_applied_flag}")
    endif()
endfunction()

radray_detect_first_compiler_flag(RADRAY_DETECTED_SIMD_FLAG
    "/arch:AVX2" "/arch:AVX" "/arch:SSE2" "/arch:SSE"
    "-mavx2" "-mavx" "-msse4.2" "-msse2" "-msse")
radray_detect_first_compiler_flag(RADRAY_DETECTED_FMA_FLAG "-mfma")
check_ipo_supported(RESULT _cmake_ipo_supported)
if (_cmake_ipo_supported)
    set_property(GLOBAL PROPERTY RADRAY_IPO_SUPPORTED TRUE)
    message(STATUS "Interprocedural optimization (IPO) is supported")
else()
    set_property(GLOBAL PROPERTY RADRAY_IPO_SUPPORTED FALSE)
endif()
unset(_cmake_ipo_supported)

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
# 直接按给定顺序探测并应用可用的 SIMD 编译标志，不做平台/编译器前端判断。
function(radray_compile_flag_auto_simd target)
    get_property(_simd_flag GLOBAL PROPERTY RADRAY_DETECTED_SIMD_FLAG)
    if (_simd_flag)
        target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:${_simd_flag}>)
    endif()
    get_property(_fma_flag GLOBAL PROPERTY RADRAY_DETECTED_FMA_FLAG)
    if (_fma_flag)
        target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:${_fma_flag}>)
    endif()
endfunction()

# radray_link_flag_lto(<target>)
# 为 target 添加链接时优化（LTO）相关的链接器标志，仅在 Release 配置下生效
function(radray_link_flag_lto target)
    get_property(_ipo_flag GLOBAL PROPERTY RADRAY_IPO_SUPPORTED)
    if (_ipo_flag AND CMAKE_BUILD_TYPE STREQUAL "Release")
        if (MSVC AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            target_link_options(${target} PRIVATE /LTCG)
        endif()
    endif()
endfunction()

# radray_compile_flag_lto(<target>)
# 为 target 添加编译时优化（LTO）相关的编译器标志，仅在 Release 配置下生效
function(radray_compile_flag_lto target)
    get_property(_ipo_flag GLOBAL PROPERTY RADRAY_IPO_SUPPORTED)
    if (_ipo_flag AND CMAKE_BUILD_TYPE STREQUAL "Release")
        set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        if (MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            target_compile_options(${target} PRIVATE -flto)
        elseif(MSVC)
            target_compile_options(${target} PRIVATE /GL)
        else()
            target_compile_options(${target} PRIVATE -flto)
        endif()
    endif()
endfunction()

function(radray_compile_flag_cpp20 target)
    if (MSVC)
        target_compile_options(${target} PRIVATE /std:c++20)
    else()
        target_compile_options(${target} PRIVATE -std=c++20)
    endif()
endfunction()

function(radray_optimize_flags_library target)
    radray_compile_flag_auto_simd(${target})
    radray_compile_flag_lto(${target})
    radray_compile_flag_cpp20(${target})
endfunction()

function(radray_optimize_flags_binary target)
    radray_compile_flag_auto_simd(${target})
    radray_compile_flag_lto(${target})
    radray_link_flag_lto(${target})
endfunction()
