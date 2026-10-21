if (NOT RADRAY_INSTALL)
    return()
endif()
include(CMakePackageConfigHelpers)
set(RADRAY_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/RadRay")
foreach(directory CMAKE_INSTALL_INCLUDEDIR CMAKE_INSTALL_LIBDIR CMAKE_INSTALL_BINDIR CMAKE_INSTALL_DATADIR)
    if (IS_ABSOLUTE "${${directory}}")
        message(FATAL_ERROR "RadRay relocatable installation requires a relative ${directory}")
    endif()
endforeach()
set(RADRAY_AVAILABLE_COMPONENTS "core;shader")
foreach(component window render shadercompiler)
    if (TARGET radray${component})
        list(APPEND RADRAY_AVAILABLE_COMPONENTS ${component})
    endif()
endforeach()
if (TARGET radray_shader_compile)
    list(APPEND RADRAY_AVAILABLE_COMPONENTS tools)
endif()

# Prevent incompatible configurations from being installed into the same prefix.
set(identity "${CMAKE_CXX_COMPILER_ID};${CMAKE_CXX_COMPILER_VERSION};${CMAKE_CXX_COMPILER};${CMAKE_SYSTEM_NAME};${CMAKE_SYSTEM_PROCESSOR};${CMAKE_SIZEOF_VOID_P};${CMAKE_GENERATOR_PLATFORM};${CMAKE_MSVC_RUNTIME_LIBRARY};${CMAKE_OSX_ARCHITECTURES};${CMAKE_POSITION_INDEPENDENT_CODE};${RADRAY_AVAILABLE_COMPONENTS};${CMAKE_INSTALL_INCLUDEDIR};${CMAKE_INSTALL_LIBDIR};${CMAKE_INSTALL_BINDIR};${CMAKE_INSTALL_DATADIR}")
get_cmake_property(variables VARIABLES)
foreach(key IN LISTS variables)
    if (key MATCHES "^RADRAY_ENABLE_" OR key MATCHES "^CMAKE_(CXX_FLAGS|C_FLAGS)($|_)")
        string(APPEND identity ";${key}=${${key}}")
    endif()
endforeach()
file(GLOB_RECURSE implementation_files CONFIGURE_DEPENDS
    "${RADRAY_SOURCE_ROOT}/modules/*/include/*.h"
    "${RADRAY_SOURCE_ROOT}/modules/*/src/*.cpp"
    "${RADRAY_SOURCE_ROOT}/modules/*/src/*.h"
    "${RADRAY_SOURCE_ROOT}/modules/*/src/*.m"
    "${RADRAY_SOURCE_ROOT}/modules/*/src/*.mm"
    "${RADRAY_SOURCE_ROOT}/tools/shader_compile/*.cpp"
    "${RADRAY_SOURCE_ROOT}/shaderlib/*.hlsli")
list(SORT implementation_files)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${implementation_files})
foreach(file IN LISTS implementation_files)
    file(RELATIVE_PATH relative "${RADRAY_SOURCE_ROOT}" "${file}")
    file(SHA256 "${file}" hash)
    string(APPEND identity ";${relative}=${hash}")
endforeach()
string(SHA256 RADRAY_INSTALL_IDENTITY "${identity}")
configure_file("${CMAKE_CURRENT_LIST_DIR}/InstallGuard.cmake.in" "${RADRAY_BINARY_ROOT}/InstallGuard.cmake" @ONLY)
install(SCRIPT "${RADRAY_BINARY_ROOT}/InstallGuard.cmake")

foreach(component IN LISTS RADRAY_AVAILABLE_COMPONENTS)
    set(RADRAY_FIND_${component} "")
    if (component STREQUAL "tools")
        install(TARGETS radray_shader_compile EXPORT RadRay_tools
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}/$<CONFIG>")
    else()
        install(TARGETS radray${component} EXPORT RadRay_${component}
            ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}/$<CONFIG>")
        set(module "${component}")
        if (component STREQUAL "shadercompiler")
            set(module shader_compiler)
        endif()
        install(DIRECTORY "${RADRAY_SOURCE_ROOT}/modules/${module}/include/radray"
            DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}" FILES_MATCHING PATTERN "*.h")
    endif()
    install(EXPORT RadRay_${component} FILE RadRay-${component}-targets.cmake
        NAMESPACE RadRay:: DESTINATION "${RADRAY_INSTALL_CMAKEDIR}")
    foreach(name IN LISTS RADRAY_DEPENDENCIES)
        set(owner "${RADRAY_DEP_${name}_component}")
        if (owner STREQUAL component OR
            (owner STREQUAL "graphics_headers" AND
                ((component STREQUAL "render" AND RADRAY_ENABLE_D3D12) OR
                 (component STREQUAL "shadercompiler" AND WIN32))))
            string(APPEND RADRAY_FIND_${component}
                "    find_dependency(${RADRAY_DEP_${name}_package} ${RADRAY_DEP_${name}_mode})\n"
                "    radray_check_package_targets(${RADRAY_DEP_${name}_package} ${RADRAY_DEP_${name}_targets})\n")
        endif()
    endforeach()
endforeach()
set(RADRAY_SHADERLIB_INSTALL_DIR "${CMAKE_INSTALL_DATADIR}/RadRay/shaderlib")
configure_package_config_file("${CMAKE_CURRENT_LIST_DIR}/RadRayConfig.cmake.in"
    "${RADRAY_BINARY_ROOT}/RadRayConfig.cmake" INSTALL_DESTINATION "${RADRAY_INSTALL_CMAKEDIR}"
    PATH_VARS RADRAY_SHADERLIB_INSTALL_DIR)
install(FILES "${RADRAY_BINARY_ROOT}/RadRayConfig.cmake"
    "${CMAKE_CURRENT_LIST_DIR}/RadRayPackageChecks.cmake"
    DESTINATION "${RADRAY_INSTALL_CMAKEDIR}")
install(DIRECTORY "${RADRAY_SOURCE_ROOT}/shaderlib/" DESTINATION "${RADRAY_SHADERLIB_INSTALL_DIR}")
install(FILES "${RADRAY_SOURCE_ROOT}/LICENSE" DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/RadRay")
