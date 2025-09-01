cmake_minimum_required(VERSION 3.23)

if (POLICY CMP0077)
    cmake_policy(SET CMP0077 NEW)
endif()

project(RadRay)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if (MSVC)
    add_compile_options(/MP)
endif()

set(RADRAY_BUILD_PATH "" CACHE PATH "RadRay build binary path")
if (NOT RADRAY_BUILD_PATH)
    if (DEFINED CMAKE_VS_MSBUILD_COMMAND)
        set(RADRAY_BUILD_PATH "${CMAKE_BINARY_DIR}/_build" CACHE PATH "RadRay build binary path" FORCE)
    else()
        set(RADRAY_BUILD_PATH "${CMAKE_BINARY_DIR}/_build/${CMAKE_BUILD_TYPE}" CACHE PATH "RadRay build binary path" FORCE)
    endif()
endif()
option(RADRAY_BUILD_TESTS "Build the RadRay tests" ON)
option(RADRAY_BUILD_BENCHMARKS "Build the RadRay benchmarks" ON)
option(RADRAY_BUILD_WINDOW "Build the RadRay window" ON)
option(RADRAY_BUILD_RENDER "Build the RadRay render" ON)
option(RADRAY_ENABLE_D3D12 "Enable Direct3D 12 backend" ON)
option(RADRAY_ENABLE_METAL "Enable Metal backend" OFF)
option(RADRAY_ENABLE_VULKAN "Enable Vulkan backend" ON)
option(RADRAY_ENABLE_MIMALLOC "Enable mimalloc as allocator" ON)
option(RADRAY_ENABLE_DXC "Enable DirectX Shader Compiler support" ON)
option(RADRAY_ENABLE_LIBPNG "Enable libpng support" ON)

function(radray_set_build_path TARGET)
    set_target_properties(${TARGET} PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY ${RADRAY_BUILD_PATH}
        LIBRARY_OUTPUT_DIRECTORY ${RADRAY_BUILD_PATH}
        RUNTIME_OUTPUT_DIRECTORY ${RADRAY_BUILD_PATH}
        PDB_OUTPUT_DIRECTORY ${RADRAY_BUILD_PATH})
endfunction()

if (NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    if (RADRAY_BUILD_BENCHMARKS)
        message(STATUS "Force disabling RADRAY_BUILD_BENCHMARKS because build type is not Release.")
    endif()
    set(RADRAY_BUILD_BENCHMARKS OFF CACHE BOOL "Build the RadRay benchmarks" FORCE)
endif()
if (NOT RADRAY_BUILD_RENDER)
    message(STATUS "RADRAY_BUILD_RENDER disabled. close all options related to rendering")
    set(RADRAY_ENABLE_D3D12 OFF CACHE BOOL "" FORCE)
    set(RADRAY_ENABLE_METAL OFF CACHE BOOL "" FORCE)
    set(RADRAY_ENABLE_VULKAN OFF CACHE BOOL "" FORCE)
    set(RADRAY_ENABLE_DXC OFF CACHE BOOL "" FORCE)
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "" FORCE)
set(FMT_MASTER_PROJECT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 11.2.0
    GIT_SHALLOW ON GIT_REMOTE_UPDATE_STRATEGY CHECKOUT
    OVERRIDE_FIND_PACKAGE)
FetchContent_MakeAvailable(fmt)
radray_set_build_path(fmt)
set(SPDLOG_MASTER_PROJECT OFF CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
set(SPDLOG_NO_EXCEPTIONS ON CACHE BOOL "" FORCE)
set(SPDLOG_NO_THREAD_ID ON CACHE BOOL "" FORCE)
set(SPDLOG_DISABLE_DEFAULT_LOGGER ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.15.3
    GIT_SHALLOW ON GIT_REMOTE_UPDATE_STRATEGY CHECKOUT)
FetchContent_MakeAvailable(spdlog)
radray_set_build_path(spdlog)
set(EIGEN_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(EIGEN_BUILD_DOC OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    eigen
    GIT_REPOSITORY https://gitlab.com/libeigen/eigen.git
    GIT_TAG 3.4
    GIT_SHALLOW ON GIT_REMOTE_UPDATE_STRATEGY CHECKOUT)
FetchContent_MakeAvailable(eigen)
set(XXHASH_BUILD_XXHSUM OFF CACHE BOOL "" FORCE)
set(DISPATCH ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    xxHash
    GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
    GIT_TAG v0.8.3
    GIT_SHALLOW ON GIT_REMOTE_UPDATE_STRATEGY CHECKOUT
    SOURCE_SUBDIR cmake_unofficial)
FetchContent_MakeAvailable(xxHash)
radray_set_build_path(xxhash)
if (RADRAY_BUILD_WINDOW)
    set(GLFW_STANDALONE OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    set(GLFW_LIBRARY_TYPE STATIC CACHE STRING "" FORCE)
    FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 3.4
        GIT_SHALLOW ON GIT_REMOTE_UPDATE_STRATEGY CHECKOUT)
    FetchContent_MakeAvailable(glfw)
    radray_set_build_path(glfw)
endif()
if (RADRAY_ENABLE_MIMALLOC)
    set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        mimalloc
        GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
        GIT_TAG v2.2.4
        GIT_SHALLOW ON GIT_REMOTE_UPDATE_STRATEGY CHECKOUT)
    FetchContent_MakeAvailable(mimalloc)
    radray_set_build_path(mimalloc-static)
endif()
if (RADRAY_ENABLE_DXC)
    set(RADRAY_DXC_VERSION "1.8.2505.1")
    set(RADRAY_DXC_HASH_WINDOWS_X64 "SHA256=97bf7e1d6fef09f90696dd37877540906c29d04be6263d7d2cd1429d56801c1b")
    set(RADRAY_DXC_HASH_LINUX_X64 "SHA256=5840824ec40e16cb83da8aa2057a5240ce59cbb2d6d84c580dabafc8d0efe534")
    set(RADRAY_DXC_HASH_MACOS_ARM64 "SHA256=5d14598aeacc9c594a33bf91e2c668231e3572c72fc32021bb4c4cd399088920")
    include(cmake/FetchDXC.cmake)
    # 构建后复制 dxc 运行时到统一输出目录
    if (RADRAY_DXC_COPY_RUNTIME AND DEFINED RADRAY_DXC_RUNTIME_DLLS)
        # VS/MSBuild 多配置生成器：按配置放入子目录
        if (DEFINED CMAKE_VS_MSBUILD_COMMAND)
            set(_DXC_RUNTIME_DEST "${RADRAY_BUILD_PATH}/$<CONFIG>")
        else()
            set(_DXC_RUNTIME_DEST "${RADRAY_BUILD_PATH}")
        endif()
        set(_DXC_COPY_CMDS COMMAND ${CMAKE_COMMAND} -E make_directory "${_DXC_RUNTIME_DEST}")
        foreach(_dll IN LISTS RADRAY_DXC_RUNTIME_DLLS)
            if (EXISTS "${_dll}")
                list(APPEND _DXC_COPY_CMDS COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_dll}" "${_DXC_RUNTIME_DEST}")
            endif()
        endforeach()
        if (_DXC_COPY_CMDS)
            add_custom_target(radray_copy_dxc_runtime ALL
                ${_DXC_COPY_CMDS}
                BYPRODUCTS ${RADRAY_DXC_RUNTIME_DLLS}
                COMMENT "Copy DXC runtime binaries to ${_DXC_RUNTIME_DEST}")
        endif()
        unset(_DXC_COPY_CMDS)
        unset(_DXC_RUNTIME_DEST)
    endif()
endif()
if (RADRAY_ENABLE_LIBPNG)
    set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZLIB_INSTALL OFF CACHE BOOL "" FORCE)
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        zlib
        GIT_REPOSITORY https://github.com/madler/zlib.git
        GIT_TAG v1.3.1
        GIT_SHALLOW ON GIT_REMOTE_UPDATE_STRATEGY CHECKOUT
        OVERRIDE_FIND_PACKAGE
        PATCH_COMMAND git apply --whitespace=fix "${CMAKE_SOURCE_DIR}/cmake/no-zlib-shared.patch")
    FetchContent_MakeAvailable(zlib)
    radray_set_build_path(zlibstatic)
endif()
if (RADRAY_ENABLE_LIBPNG)
    if (TARGET zlibstatic AND NOT TARGET ZLIB::ZLIB)
        add_library(ZLIB::ZLIB ALIAS zlibstatic)
    endif()
    set(PNG_TESTS OFF CACHE BOOL "" FORCE)
    set(PNG_TOOLS OFF CACHE BOOL "" FORCE)
    set(PNG_SHARED OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        libpng
        GIT_REPOSITORY https://github.com/pnggroup/libpng.git
        GIT_TAG v1.6.50
        GIT_SHALLOW ON GIT_REMOTE_UPDATE_STRATEGY CHECKOUT)
    FetchContent_MakeAvailable(libpng)
    radray_set_build_path(png_static)
endif()
if (RADRAY_ENABLE_VULKAN)
    set(PROJECT_IS_TOP_LEVEL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        Vulkan-Headers
        GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
        GIT_TAG v1.4.321
        GIT_SHALLOW ON GIT_REMOTE_UPDATE_STRATEGY CHECKOUT)
    FetchContent_MakeAvailable(Vulkan-Headers)
    if (DEFINED vulkan-headers_SOURCE_DIR)
        set(VULKAN_HEADERS_INSTALL_DIR "${vulkan-headers_SOURCE_DIR}" CACHE PATH "" FORCE)
    endif()
    set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "" FORCE)
    set(VOLK_HEADERS_ONLY ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        volk
        GIT_REPOSITORY https://github.com/zeux/volk.git
        GIT_TAG 1.4.304
        GIT_SHALLOW ON GIT_REMOTE_UPDATE_STRATEGY CHECKOUT)
    FetchContent_MakeAvailable(volk)
    FetchContent_Declare(
        VulkanMemoryAllocator
        GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
        GIT_TAG v3.3.0
        GIT_SHALLOW ON GIT_REMOTE_UPDATE_STRATEGY CHECKOUT)
    FetchContent_MakeAvailable(VulkanMemoryAllocator)
endif()
if (DEFINED RADRAY_ENABLE_DXC OR DEFINED RADRAY_ENABLE_D3D12)
    set(DXHEADERS_BUILD_TEST OFF CACHE BOOL "" FORCE)
    set(DXHEADERS_INSTALL OFF CACHE BOOL "" FORCE)
    set(DXHEADERS_BUILD_GOOGLE_TEST OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        DirectX-Headers
        GIT_REPOSITORY https://github.com/microsoft/DirectX-Headers.git
        GIT_TAG v1.616.0
        GIT_SHALLOW ON GIT_REMOTE_UPDATE_STRATEGY CHECKOUT)
    FetchContent_MakeAvailable(DirectX-Headers)
    FetchContent_Declare(
        D3D12MemoryAllocator
        GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator.git
        GIT_TAG v3.0.1
        GIT_SHALLOW ON GIT_REMOTE_UPDATE_STRATEGY CHECKOUT)
    FetchContent_MakeAvailable(D3D12MemoryAllocator)
    target_compile_options(D3D12MemoryAllocator PRIVATE 
        $<$<AND:$<CXX_COMPILER_ID:Clang>,$<BOOL:${MSVC}>>:-Wno-unused-variable>
        $<$<AND:$<CXX_COMPILER_ID:Clang>,$<BOOL:${MSVC}>>:-Wno-switch>
        $<$<AND:$<CXX_COMPILER_ID:Clang>,$<BOOL:${MSVC}>>:-Wno-unused-const-variable>
        $<$<AND:$<CXX_COMPILER_ID:Clang>,$<BOOL:${MSVC}>>:-Wno-unused-function>)
endif()
if (RADRAY_BUILD_TESTS)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.17.0
        GIT_SHALLOW ON GIT_REMOTE_UPDATE_STRATEGY CHECKOUT)
    FetchContent_MakeAvailable(googletest)
    radray_set_build_path(gtest_main)
    if (RADRAY_BUILD_BENCHMARKS)
        set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_INSTALL_TOOLS OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_USE_BUNDLED_GTEST OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            googlebenchmark
            GIT_REPOSITORY https://github.com/google/benchmark.git
            GIT_TAG v1.9.4
            GIT_SHALLOW ON GIT_REMOTE_UPDATE_STRATEGY CHECKOUT)
        FetchContent_MakeAvailable(googlebenchmark)
        radray_set_build_path(benchmark)
    endif()
endif()

add_subdirectory(modules)
if (RADRAY_BUILD_TESTS)
    enable_testing()
    include(GoogleTest)
    include(cmake/ExampleHelper.cmake)
    add_subdirectory(tests)
    add_subdirectory(examples)
    if (RADRAY_BUILD_BENCHMARKS)
        add_subdirectory(benchmarks)
    endif()
endif()
