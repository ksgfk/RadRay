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

# 消费 target 只需与集中式部署 target 建立构建依赖, 不再各自 POST_BUILD 拷贝
# (见 CMakeLists.txt 的 radray_dxc_runtime_deploy)。DLL 与所有 radray 二进制同处
# ${RADRAY_BUILD_PATH}/$<CONFIG>, 裸名动态加载即可命中。
function(radray_deploy_dxc_runtime TARGET)
    if (NOT RADRAY_BUILD_SHADER_COMPILER)
        return()
    endif()
    if (NOT TARGET radray_dxc_runtime_deploy)
        message(FATAL_ERROR "radray_deploy_dxc_runtime: radray_dxc_runtime_deploy target is missing")
    endif()
    add_dependencies(${TARGET} radray_dxc_runtime_deploy)
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
        target_compile_options(${target} PRIVATE /permissive- /utf-8 /Zc:preprocessor /Zc:__cplusplus /W4 /wd4324 /EHsc)
        target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/GR>)
    else()
        target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-frtti>)
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

# radray_gtest_discover_tests(<target> [gtest_discover_tests extra args except DISCOVERY_MODE])
# 构建期 POST_BUILD 发现, 把每个 TEST() 注册成独立 CTest 用例. 不用 CMake 4.4 的 PRE_TEST:
# 它每次 ctest 都对全部测试 exe 跑 --gtest_list_tests, 无缓存. 也不直接调用
# gtest_discover_tests(POST_BUILD): 同目录多目标并行时 JSON 路径只靠 TEST_TARGET 哈希,
# 哈希为空就会争用同一文件. 这里把 JSON 目录固定为每目标独立路径.
function(radray_gtest_discover_tests target)
    set(_options NO_PRETTY_TYPES NO_PRETTY_VALUES)
    set(_one_value_args TEST_PREFIX TEST_SUFFIX WORKING_DIRECTORY TEST_LIST DISCOVERY_TIMEOUT XML_OUTPUT_DIR DISCOVERY_MODE)
    set(_multi_value_args EXTRA_ARGS DISCOVERY_EXTRA_ARGS PROPERTIES TEST_FILTER)
    cmake_parse_arguments(PARSE_ARGV 1 arg "${_options}" "${_one_value_args}" "${_multi_value_args}")

    if (arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "radray_gtest_discover_tests: unexpected arguments: ${arg_UNPARSED_ARGUMENTS}")
    endif()
    if (arg_DISCOVERY_MODE AND NOT arg_DISCOVERY_MODE STREQUAL "POST_BUILD")
        message(FATAL_ERROR "radray_gtest_discover_tests: DISCOVERY_MODE must be POST_BUILD")
    endif()
    if (NOT TARGET ${target})
        message(FATAL_ERROR "radray_gtest_discover_tests: target '${target}' does not exist")
    endif()
    if (NOT arg_WORKING_DIRECTORY)
        set(arg_WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    endif()
    if (NOT arg_TEST_LIST)
        set(arg_TEST_LIST ${target}_TESTS)
    endif()
    if (NOT arg_DISCOVERY_TIMEOUT)
        set(arg_DISCOVERY_TIMEOUT 30)
    endif()
    if (arg_PROPERTIES)
        list(LENGTH arg_PROPERTIES _len_PROPERTIES)
        math(EXPR _odd_PROPERTIES "${_len_PROPERTIES} % 2")
        if (_odd_PROPERTIES)
            message(FATAL_ERROR "radray_gtest_discover_tests: PROPERTIES must be key-value pairs")
        endif()
    endif()

    set(ctest_file_base "${CMAKE_CURRENT_BINARY_DIR}/${target}")
    set(json_dir "${CMAKE_CURRENT_BINARY_DIR}/gtest_discovery/${target}")
    get_property(_radray_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if (_radray_multi_config)
        string(APPEND ctest_file_base "_$<CONFIG>")
        string(APPEND json_dir "/$<CONFIG>")
    endif()

    set(ctest_include_file "${ctest_file_base}_include.cmake")
    set(discovery_file "${ctest_file_base}_discovery.cmake")
    set(ctest_tests_file "${ctest_file_base}_tests.cmake")

    if (NOT DEFINED arg_EXTRA_ARGS)
        set(arg_EXTRA_ARGS "")
    endif()
    if (NOT DEFINED arg_PROPERTIES)
        set(arg_PROPERTIES "")
    endif()
    if (NOT DEFINED arg_DISCOVERY_EXTRA_ARGS)
        set(arg_DISCOVERY_EXTRA_ARGS "")
    endif()
    list(JOIN arg_EXTRA_ARGS "]==] [==[" arg_EXTRA_ARGS)
    list(JOIN arg_PROPERTIES "]==] [==[" arg_PROPERTIES)
    list(JOIN arg_DISCOVERY_EXTRA_ARGS "]==] [==[" arg_DISCOVERY_EXTRA_ARGS)

    add_custom_command(
        TARGET ${target} POST_BUILD
        BYPRODUCTS "${ctest_tests_file}"
        COMMAND "${CMAKE_COMMAND}" -P "${discovery_file}"
        COMMENT "Discovering gtest cases for ${target}"
    )

    string(CONCAT discovery_content
        "file(MAKE_DIRECTORY [==[${json_dir}]==])"                            "\n"
        "include(\"${CMAKE_ROOT}/Modules/GoogleTestAddTests.cmake\")"         "\n"
        "gtest_discover_tests_impl("                                          "\n"
        "  TEST_TARGET"            " [==[${target}]==]"                       "\n"
        "  TEST_EXECUTABLE"        " [==[$<TARGET_FILE:${target}>]==]"        "\n"
        "  TEST_EXECUTOR"          " [==[]==]"                                 "\n"
        "  TEST_WORKING_DIR"       " [==[${arg_WORKING_DIRECTORY}]==]"        "\n"
        "  TEST_EXTRA_ARGS"        " [==[${arg_EXTRA_ARGS}]==]"               "\n"
        "  TEST_PROPERTIES"        " [==[${arg_PROPERTIES}]==]"               "\n"
        "  TEST_PREFIX"            " [==[${arg_TEST_PREFIX}]==]"              "\n"
        "  TEST_SUFFIX"            " [==[${arg_TEST_SUFFIX}]==]"              "\n"
        "  TEST_FILTER"            " [==[${arg_TEST_FILTER}]==]"              "\n"
        "  NO_PRETTY_TYPES"        " [==[${arg_NO_PRETTY_TYPES}]==]"          "\n"
        "  NO_PRETTY_VALUES"       " [==[${arg_NO_PRETTY_VALUES}]==]"         "\n"
        "  TEST_LIST"              " [==[${arg_TEST_LIST}]==]"                "\n"
        "  CTEST_FILE"             " [==[${ctest_tests_file}]==]"             "\n"
        "  TEST_DISCOVERY_TIMEOUT" " [==[${arg_DISCOVERY_TIMEOUT}]==]"        "\n"
        "  TEST_DISCOVERY_EXTRA_ARGS [==[${arg_DISCOVERY_EXTRA_ARGS}]==]"     "\n"
        "  TEST_XML_OUTPUT_DIR"    " [==[${arg_XML_OUTPUT_DIR}]==]"           "\n"
        "  TEST_JSON_OUTPUT_DIR"   " [==[${json_dir}]==]"                     "\n"
        ")"                                                                   "\n"
    )
    file(GENERATE OUTPUT "${discovery_file}" CONTENT "${discovery_content}")

    string(CONCAT ctest_include_content
        "if(EXISTS \"${ctest_tests_file}\")"                                  "\n"
        "  include(\"${ctest_tests_file}\")"                                  "\n"
        "else()"                                                              "\n"
        "  add_test(${target}_NOT_BUILT ${target}_NOT_BUILT)"                 "\n"
        "endif()"                                                             "\n"
    )
    file(GENERATE OUTPUT "${ctest_include_file}" CONTENT "${ctest_include_content}")

    if (_radray_multi_config)
        string(REPLACE [[_$<CONFIG>]] [[_${CTEST_CONFIGURATION_TYPE}]]
            include_file_cfg "${ctest_include_file}")
        string(REPLACE [[_$<CONFIG>]] "" ctest_include_file "${ctest_include_file}")
        file(WRITE "${ctest_include_file}"
            "if(EXISTS \"${include_file_cfg}\")"                              "\n"
            "  include(\"${include_file_cfg}\")"                              "\n"
            "endif()"                                                         "\n"
        )
    endif()

    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${ctest_include_file}")
endfunction()

# radray_add_test(<target> SOURCES <src...> [LINK_LIBS <libs...>] [DISCOVER_ARGS <args...>] [COMPILE_OPTIONS <opts...>] [NO_DISCOVER])
function(radray_add_test target)
    set(_options NO_DISCOVER)
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
    if (NOT RADRAY_TEST_NO_DISCOVER)
        radray_gtest_discover_tests(${target} ${RADRAY_TEST_DISCOVER_ARGS})
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
