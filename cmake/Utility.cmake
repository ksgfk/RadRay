# Guard against multiple inclusion
if (DEFINED RADRAY_CMAKE_UTILITY_INCLUDED)
    return()
endif()
set(RADRAY_CMAKE_UTILITY_INCLUDED TRUE)

include(CheckCXXCompilerFlag)
include(CheckIPOSupported)
include(CMakeParseArguments)

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

radray_detect_first_compiler_flag(RADRAY_DETECTED_SIMD_FLAG
    "/arch:AVX2" "/arch:AVX" "/arch:SSE2" "/arch:SSE"
    "-mcpu=native")
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
        ARCHIVE_OUTPUT_DIRECTORY "${RADRAY_BUILD_PATH}/$<CONFIG>"
        LIBRARY_OUTPUT_DIRECTORY "${RADRAY_BUILD_PATH}/$<CONFIG>"
        RUNTIME_OUTPUT_DIRECTORY "${RADRAY_BUILD_PATH}/$<CONFIG>"
        PDB_OUTPUT_DIRECTORY "${RADRAY_BUILD_PATH}/$<CONFIG>")
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
    if (_ipo_flag)
        if (MSVC AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            target_link_options(${target} PRIVATE $<$<CONFIG:Release>:/LTCG>)
        endif()
    endif()
endfunction()

# radray_compile_flag_lto(<target>)
# 为 target 添加编译时优化（LTO）相关的编译器标志，仅在 Release 配置下生效
function(radray_compile_flag_lto target)
    get_property(_ipo_flag GLOBAL PROPERTY RADRAY_IPO_SUPPORTED)
    if (_ipo_flag)
        set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        if (MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:-flto>)
        elseif(MSVC)
            target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:/GL>)
        else()
            target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:-flto>)
        endif()
    endif()
endfunction()

function(radray_compile_flag_cpp20 target)
    if (MSVC)
        target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/std:c++20>)
    else()
        target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=c++20>)
    endif()
endfunction()

function(radray_default_compile_flags target)
    if (WIN32)
        target_compile_definitions(${target} PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    if (MSVC)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(${target} PRIVATE /permissive- /utf-8 /Zc:preprocessor /Zc:__cplusplus /W4 /wd4324)
    endif()
    target_compile_definitions(${target} PRIVATE
        $<$<PLATFORM_ID:Windows>:RADRAY_PLATFORM_WINDOWS>
        $<$<PLATFORM_ID:Darwin>:RADRAY_PLATFORM_MACOS>
        $<$<PLATFORM_ID:iOS>:RADRAY_PLATFORM_IOS>
        $<$<OR:$<PLATFORM_ID:Darwin>,$<PLATFORM_ID:iOS>>:RADRAY_PLATFORM_APPLE>
        $<$<NOT:$<CONFIG:Release>>:RADRAY_IS_DEBUG>)
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

# radray_add_test(<target> SOURCES <src...> [LINK_LIBS <libs...>] [DISCOVER_ARGS <args...>] [COMPILE_OPTIONS <opts...>])
function(radray_add_test target)
    set(_options)
    set(_one_value_args)
    set(_multi_value_args SOURCES LINK_LIBS DISCOVER_ARGS COMPILE_OPTIONS)
    cmake_parse_arguments(RADRAY_TEST "${_options}" "${_one_value_args}" "${_multi_value_args}" ${ARGN})

    if (NOT RADRAY_TEST_SOURCES)
        message(FATAL_ERROR "radray_add_test: SOURCES is required for target ${target}")
    endif()

    add_executable(${target} ${RADRAY_TEST_SOURCES})
    target_link_libraries(${target} PRIVATE ${RADRAY_TEST_LINK_LIBS} GTest::gtest_main)
    if (RADRAY_TEST_COMPILE_OPTIONS)
        target_compile_options(${target} PRIVATE ${RADRAY_TEST_COMPILE_OPTIONS})
    endif()
    if (RADRAY_TEST_DISCOVER_ARGS)
        gtest_discover_tests(${target} ${RADRAY_TEST_DISCOVER_ARGS})
    else()
        gtest_discover_tests(${target})
    endif()
    radray_default_compile_flags(${target})
    radray_optimize_flags_binary(${target})
    radray_set_build_path(${target})
endfunction()

# radray_add_example(<target> SOURCES <src...> [LINK_LIBS <libs...>] [COMPILE_OPTIONS <opts...>])
function(radray_add_example target)
    set(_options)
    set(_one_value_args)
    set(_multi_value_args SOURCES LINK_LIBS COMPILE_OPTIONS)
    cmake_parse_arguments(RADRAY_EXAMPLE "${_options}" "${_one_value_args}" "${_multi_value_args}" ${ARGN})

    if (NOT RADRAY_EXAMPLE_SOURCES)
        message(FATAL_ERROR "radray_add_example: SOURCES is required for target ${target}")
    endif()

    add_executable(${target} ${RADRAY_EXAMPLE_SOURCES})
    target_link_libraries(${target} PRIVATE ${RADRAY_EXAMPLE_LINK_LIBS})
    if (RADRAY_EXAMPLE_COMPILE_OPTIONS)
        target_compile_options(${target} PRIVATE ${RADRAY_EXAMPLE_COMPILE_OPTIONS})
    endif()
    radray_default_compile_flags(${target})
    radray_optimize_flags_binary(${target})
    radray_set_build_path(${target})
endfunction()

# radray_add_gtest_case(<test_name> TARGET <target> FILTER <gtest_filter> [ALSO_RUN_DISABLED] [WORKING_DIRECTORY <dir>] [ENV <key=value>...])
function(radray_add_gtest_case test_name)
    set(_options ALSO_RUN_DISABLED)
    set(_one_value_args TARGET FILTER WORKING_DIRECTORY)
    set(_multi_value_args ENV)
    cmake_parse_arguments(RADRAY_GTEST_CASE "${_options}" "${_one_value_args}" "${_multi_value_args}" ${ARGN})

    if (NOT RADRAY_GTEST_CASE_TARGET)
        message(FATAL_ERROR "radray_add_gtest_case: TARGET is required for test ${test_name}")
    endif()
    if (NOT RADRAY_GTEST_CASE_FILTER)
        message(FATAL_ERROR "radray_add_gtest_case: FILTER is required for test ${test_name}")
    endif()

    set(_cmd ${RADRAY_GTEST_CASE_TARGET} "--gtest_filter=${RADRAY_GTEST_CASE_FILTER}")
    if (RADRAY_GTEST_CASE_ALSO_RUN_DISABLED)
        list(APPEND _cmd "--gtest_also_run_disabled_tests")
    endif()
    add_test(NAME ${test_name} COMMAND ${_cmd})

    if (RADRAY_GTEST_CASE_WORKING_DIRECTORY)
        set_tests_properties(${test_name} PROPERTIES WORKING_DIRECTORY "${RADRAY_GTEST_CASE_WORKING_DIRECTORY}")
    endif()
    if (RADRAY_GTEST_CASE_ENV)
        set_tests_properties(${test_name} PROPERTIES ENVIRONMENT "${RADRAY_GTEST_CASE_ENV}")
    endif()
endfunction()

function(radray_add_radray_gtest_case test_name)
    set(_options ALSO_RUN_DISABLED)
    set(_one_value_args TARGET FILTER WORKING_DIRECTORY TEST_ENV_DIR TEST_ARTIFACTS_DIR UPDATE_BASELINE)
    set(_multi_value_args ENV)
    cmake_parse_arguments(RADRAY_CASE "${_options}" "${_one_value_args}" "${_multi_value_args}" ${ARGN})

    if (NOT RADRAY_CASE_TEST_ENV_DIR)
        set(RADRAY_CASE_TEST_ENV_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    if (NOT RADRAY_CASE_TEST_ARTIFACTS_DIR)
        set(RADRAY_CASE_TEST_ARTIFACTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    if (NOT DEFINED RADRAY_CASE_UPDATE_BASELINE)
        set(RADRAY_CASE_UPDATE_BASELINE 0)
    endif()

    set(_radray_common_env
        "RADRAY_PROJECT_DIR=${CMAKE_SOURCE_DIR}"
        "RADRAY_TEST_ENV_DIR=${RADRAY_CASE_TEST_ENV_DIR}"
        "RADRAY_ASSETS_DIR=${CMAKE_SOURCE_DIR}/assets"
        "RADRAY_TEST_ARTIFACTS_DIR=${RADRAY_CASE_TEST_ARTIFACTS_DIR}"
        "RADRAY_TEST_UPDATE_BASELINE=${RADRAY_CASE_UPDATE_BASELINE}")
    if (RADRAY_CASE_ENV)
        list(APPEND _radray_common_env ${RADRAY_CASE_ENV})
    endif()

    set(_radray_case_args
        TARGET ${RADRAY_CASE_TARGET}
        FILTER ${RADRAY_CASE_FILTER}
        ENV ${_radray_common_env})
    if (RADRAY_CASE_WORKING_DIRECTORY)
        list(APPEND _radray_case_args WORKING_DIRECTORY "${RADRAY_CASE_WORKING_DIRECTORY}")
    endif()
    if (RADRAY_CASE_ALSO_RUN_DISABLED)
        list(APPEND _radray_case_args ALSO_RUN_DISABLED)
    endif()
    radray_add_gtest_case(${test_name} ${_radray_case_args})
endfunction()
