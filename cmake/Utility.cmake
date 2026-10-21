include_guard(GLOBAL)

include(CheckCXXCompilerFlag)
include(CheckIPOSupported)
include(CMakeParseArguments)

# 探测第一个可用的编译器标志并写入全局属性
function(radray_detect_first_compiler_flag GLOBAL_PROPERTY)
    set(_applied_flag "")
    foreach(_flag IN LISTS ARGN)
        string(REGEX REPLACE "[^A-Za-z0-9_]" "_" _flag_key "${_flag}")
        set(_var_name "RADRAY_HAVE_FLAG_${_flag_key}")
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

if (RADRAY_ENABLE_AUTO_OPTIMIZATION)
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
endif()

function(radray_set_build_path TARGET)
    set_target_properties(${TARGET} PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${RADRAY_BUILD_PATH}/$<CONFIG>"
        LIBRARY_OUTPUT_DIRECTORY "${RADRAY_BUILD_PATH}/$<CONFIG>"
        RUNTIME_OUTPUT_DIRECTORY "${RADRAY_BUILD_PATH}/$<CONFIG>"
        PDB_OUTPUT_DIRECTORY "${RADRAY_BUILD_PATH}/$<CONFIG>")
endfunction()

# radray_compile_flag_auto_simd(<target>)
# 直接按给定顺序探测并应用可用的 SIMD 编译标志，不做平台/编译器前端判断。
function(radray_compile_flag_auto_simd target)
    if (NOT RADRAY_ENABLE_AUTO_OPTIMIZATION)
        return()
    endif()
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
    if (NOT RADRAY_ENABLE_AUTO_OPTIMIZATION)
        return()
    endif()
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
    if (NOT RADRAY_ENABLE_AUTO_OPTIMIZATION)
        return()
    endif()
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
endfunction()

function(radray_optimize_flags_library target)
    radray_compile_flag_auto_simd(${target})
    radray_compile_flag_lto(${target})
endfunction()

function(radray_optimize_flags_binary target)
    radray_compile_flag_auto_simd(${target})
    radray_compile_flag_lto(${target})
    radray_link_flag_lto(${target})
endfunction()

# 默认把多个 .cpp 合成一批编译；显式指定时遵从 CMAKE_UNITY_BUILD。
function(radray_enable_unity_build target)
    set(_unity_build ON)
    if (DEFINED CMAKE_UNITY_BUILD)
        set(_unity_build "${CMAKE_UNITY_BUILD}")
    endif()
    set_target_properties(${target} PROPERTIES
        UNITY_BUILD "${_unity_build}"
        UNITY_BUILD_BATCH_SIZE 32)
    if (MSVC)
        target_compile_options(${target} PRIVATE /bigobj)
    endif()
endfunction()

# Ninja/Makefile 配置完成后显式调用，不依赖任何 C++ 构建目标。
function(radray_add_compile_commands_target)
    if (NOT CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
        return()
    endif()
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    add_custom_target(radray_compile_commands
        COMMAND "${Python3_EXECUTABLE}" "${RADRAY_SOURCE_ROOT}/tools/expand_unity_compile_commands.py"
            "${CMAKE_BINARY_DIR}/compile_commands.json"
            --output "${RADRAY_SOURCE_ROOT}/.vscode/compile_commands.json"
        WORKING_DIRECTORY "${RADRAY_SOURCE_ROOT}"
        COMMENT "Updating per-source compile commands for clangd"
        VERBATIM
        USES_TERMINAL)
endfunction()
