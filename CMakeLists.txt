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

option(RADRAY_BUILD_TESTS "Build the RadRay tests" OFF)
option(RADRAY_BUILD_BENCHMARKS "Build the RadRay benchmarks" OFF)
option(RADRAY_BUILD_WINDOW "Build the RadRay window" OFF)
option(RADRAY_ENABLE_D3D12 "Enable Direct3D 12 backend" OFF)
option(RADRAY_ENABLE_METAL "Enable Metal backend" OFF)
option(RADRAY_ENABLE_VULKAN "Enable Vulkan backend" ON)
option(RADRAY_ENABLE_MIMALLOC "Enable mimalloc as allocator" ON)
option(RADRAY_ENABLE_DXC "Enable DirectX Shader Compiler support" ON)
option(RADRAY_ENABLE_LIBPNG "Enable libpng support" ON)

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
endif()
if (RADRAY_ENABLE_DXC)
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
endif()

add_subdirectory(modules)
