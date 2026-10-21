cmake_minimum_required(VERSION 3.25)

if (NOT DEFINED RADRAY_BUILD_DIR OR RADRAY_BUILD_DIR STREQUAL "")
    message(FATAL_ERROR "RADRAY_BUILD_DIR must name a configured build directory")
endif()

set(_cache "${RADRAY_BUILD_DIR}/CMakeCache.txt")
if (NOT EXISTS "${_cache}")
    message(FATAL_ERROR "CMake cache does not exist: ${_cache}")
endif()
file(STRINGS "${_cache}" _generator_entry REGEX "^CMAKE_GENERATOR:INTERNAL=")
if (NOT _generator_entry MATCHES "^CMAKE_GENERATOR:INTERNAL=.+")
    message(FATAL_ERROR "CMake generator is missing from ${_cache}")
endif()
if (NOT _generator_entry MATCHES "Ninja|Makefiles")
    message(STATUS "Skipping unity database expansion for this generator; MSBuild uses win_gen_compile_commands.py")
    return()
endif()

set(_config_args)
if (DEFINED RADRAY_BUILD_CONFIG AND NOT RADRAY_BUILD_CONFIG STREQUAL "")
    list(APPEND _config_args --config "${RADRAY_BUILD_CONFIG}")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${RADRAY_BUILD_DIR}" ${_config_args}
        --target radray_compile_commands
    COMMAND_ERROR_IS_FATAL ANY)
