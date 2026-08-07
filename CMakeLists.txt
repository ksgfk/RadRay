cmake_minimum_required(VERSION 3.25)

if (POLICY CMP0077)
    cmake_policy(SET CMP0077 NEW)
endif()

project(RadRay)

if (APPLE)
    enable_language(OBJC OBJCXX)
endif()

include(cmake/Utility.cmake)
include(ExternalProject)

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# 设置全局编译器选项
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# 本项目不使用 C++20 modules, 关闭模块依赖扫描.
# 必须在任何 add_subdirectory 之前设置, 否则 third_party 目标会继承 ON.
# 原因: CMP0155 在 cmake_minimum_required(VERSION 3.25) 下为 OLD, 但 CMake 4.4 的
# Visual Studio 生成器仍会对声明 C++20 的目标写出 ScanSourceForModuleDependencies=true,
# 而 ClangCL 工具链不认识 MSBuild 传入的 /scanDependencies, 导致 .obj 根本不生成,
# 随后 llvm-lib 以 MSB6006 失败. 增量构建会因残留的 .lib 掩盖该错误, 只有干净构建才暴露.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
if (MSVC)
    add_compile_options(/MP)
endif()

# 设置全局构建输出路径
set(RADRAY_BUILD_PATH "" CACHE PATH "RadRay build binary path")
if (NOT RADRAY_BUILD_PATH)
    set(RADRAY_BUILD_PATH "${CMAKE_BINARY_DIR}/_build" CACHE PATH "RadRay build binary path" FORCE)
endif()

# 编译配置项
include(CMakeDependentOption)
option(RADRAY_BUILD_TESTS "Build the RadRay tests" ON)
set(_radray_benchmarks_default OFF)
if (CMAKE_CONFIGURATION_TYPES)
    list(FIND CMAKE_CONFIGURATION_TYPES "Release" _radray_release_cfg_index)
    if (NOT _radray_release_cfg_index EQUAL -1)
        set(_radray_benchmarks_default ON)
    endif()
elseif (CMAKE_BUILD_TYPE STREQUAL "Release")
    set(_radray_benchmarks_default ON)
endif()
option(RADRAY_BUILD_BENCHMARKS "Build the RadRay benchmarks (enabled by default for Release-capable configs)" ${_radray_benchmarks_default})
unset(_radray_benchmarks_default)
unset(_radray_release_cfg_index)
option(RADRAY_ENABLE_MIMALLOC "Enable mimalloc as allocator" ON)
option(RADRAY_ENABLE_ZLIB "Enable zlib support" ON)
cmake_dependent_option(RADRAY_ENABLE_LIBPNG "Enable libpng support" ON "RADRAY_ENABLE_ZLIB" OFF)
option(RADRAY_ENABLE_LIBJPEG "Enable libjpeg-turbo support" ON)
option(RADRAY_BUILD_WINDOW "Build the RadRay window" ON)
option(RADRAY_BUILD_RENDER "Build the RadRay render" ON)
cmake_dependent_option(RADRAY_BUILD_RUNTIME "Build the RadRay runtime" ON "RADRAY_BUILD_RENDER AND RADRAY_BUILD_WINDOW" OFF)
cmake_dependent_option(RADRAY_BUILD_EXAMPLES "Build the RadRay examples" ON "RADRAY_BUILD_RUNTIME" OFF)
cmake_dependent_option(RADRAY_ENABLE_D3D12 "Enable Direct3D 12 backend" ON "RADRAY_BUILD_RENDER AND WIN32" OFF)
cmake_dependent_option(RADRAY_ENABLE_VULKAN "Enable Vulkan backend" ON "RADRAY_BUILD_RENDER" OFF)
option(RADRAY_BUILD_SHADER_COMPILER "Enable RadRay DXC fork compiler capability" ON)
cmake_dependent_option(RADRAY_ENABLE_SHADER_JIT "Enable runtime shader JIT orchestration" ON "RADRAY_BUILD_SHADER_COMPILER" OFF)
cmake_dependent_option(RADRAY_BUILD_SHADER_TOOLS "Build RadRay shader command line tools" ON "RADRAY_BUILD_SHADER_COMPILER" OFF)

set(RADRAY_THIRDPARTY_ROOT "${CMAKE_SOURCE_DIR}/third_party")

# 配置 third party
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
include(FetchContent)

set(FETCHCONTENT_UPDATES_DISCONNECTED OFF CACHE BOOL "" FORCE)
set(FMT_MASTER_PROJECT OFF CACHE BOOL "" FORCE)
set(FMT_MODULE OFF CACHE BOOL "" FORCE)
add_subdirectory(${RADRAY_THIRDPARTY_ROOT}/fmt)
radray_optimize_flags_library(fmt)
radray_set_build_path(fmt)

add_subdirectory(${RADRAY_THIRDPARTY_ROOT}/magic_enum)

set(SPDLOG_MASTER_PROJECT OFF CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
if (WIN32)
    set(SPDLOG_WCHAR_SUPPORT ON CACHE BOOL "" FORCE)
endif()
set(SPDLOG_NO_EXCEPTIONS ON CACHE BOOL "" FORCE)
set(SPDLOG_NO_THREAD_ID ON CACHE BOOL "" FORCE)
set(SPDLOG_DISABLE_DEFAULT_LOGGER ON CACHE BOOL "" FORCE)
add_subdirectory(${RADRAY_THIRDPARTY_ROOT}/spdlog)
radray_set_build_path(spdlog)

set(PROJECT_IS_TOP_LEVEL OFF CACHE BOOL "" FORCE)
set(EIGEN_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(EIGEN_BUILD_DOC OFF CACHE BOOL "" FORCE)
add_subdirectory(${RADRAY_THIRDPARTY_ROOT}/eigen)

set(XXHASH_BUILD_XXHSUM OFF CACHE BOOL "" FORCE)
set(DISPATCH ON CACHE BOOL "" FORCE)
add_subdirectory(${RADRAY_THIRDPARTY_ROOT}/xxHash/build/cmake)
radray_optimize_flags_library(xxhash)
radray_set_build_path(xxhash)

set(YYJSON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YYJSON_BUILD_MISC OFF CACHE BOOL "" FORCE)
set(YYJSON_BUILD_DOC OFF CACHE BOOL "" FORCE)
set(YYJSON_DISABLE_UNALIGNED_MEMORY_ACCESS ON CACHE BOOL "" FORCE)
add_subdirectory(${RADRAY_THIRDPARTY_ROOT}/yyjson)
radray_optimize_flags_library(yyjson)
radray_set_build_path(yyjson)

add_subdirectory(${RADRAY_THIRDPARTY_ROOT}/sigslot)

# cgltf 纯头文件库
add_library(cgltf INTERFACE)
add_library(cgltf::cgltf ALIAS cgltf)
target_include_directories(cgltf SYSTEM INTERFACE "${RADRAY_THIRDPARTY_ROOT}/cgltf")

# stdexec 纯头文件库
add_library(stdexec INTERFACE)
add_library(STDEXEC::stdexec ALIAS stdexec)
target_include_directories(stdexec SYSTEM INTERFACE "${RADRAY_THIRDPARTY_ROOT}/stdexec/include")
target_compile_features(stdexec INTERFACE cxx_std_20)

if (RADRAY_ENABLE_MIMALLOC)
    set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(MI_XMALLOC ON CACHE BOOL "" FORCE)
    set(MI_OPT_ARCH ON CACHE BOOL "" FORCE)
    add_subdirectory(${RADRAY_THIRDPARTY_ROOT}/mimalloc)
    radray_optimize_flags_library(mimalloc-static)
    radray_set_build_path(mimalloc-static)
    # INTERFACE 库强制 whole-archive 链接, 保证符号重载都在最终产物内
    add_library(mimalloc-override INTERFACE)
    add_dependencies(mimalloc-override mimalloc-static)
    target_include_directories(mimalloc-override INTERFACE
        $<TARGET_PROPERTY:mimalloc-static,INTERFACE_INCLUDE_DIRECTORIES>)
    if (APPLE)
        target_link_options(mimalloc-override INTERFACE
            "LINKER:-force_load,$<TARGET_FILE:mimalloc-static>")
    elseif (MSVC)
        target_link_options(mimalloc-override INTERFACE
            "/WHOLEARCHIVE:$<TARGET_FILE:mimalloc-static>")
    else()
        target_link_options(mimalloc-override INTERFACE
            "LINKER:--whole-archive" $<TARGET_FILE:mimalloc-static> "LINKER:--no-whole-archive")
    endif()
endif()

if (RADRAY_BUILD_SHADER_COMPILER)
    set(_DXC_ROOT "${CMAKE_SOURCE_DIR}/SDKs/radray_dxc/extracted")
    if (NOT EXISTS "${_DXC_ROOT}")
        message(FATAL_ERROR
            "RadRayDXC fork package root does not exist: ${_DXC_ROOT}\n"
            "Run: python tools/fetch_sdks.py restore")
    endif()

    find_package(RadRayDXC CONFIG REQUIRED COMPONENTS Headers Compiler
        PATHS "${_DXC_ROOT}/lib/cmake/RadRayDXC"
        NO_DEFAULT_PATH)
    if (NOT TARGET RadRayDXC::Compiler)
        message(FATAL_ERROR "RadRayDXC package did not provide RadRayDXC::Compiler")
    endif()

    get_target_property(_DXC_INCLUDE_DIR RadRayDXC::Headers INTERFACE_INCLUDE_DIRECTORIES)
    set(RADRAY_DXC_INCLUDE_DIR "${_DXC_INCLUDE_DIR}" CACHE INTERNAL "DXC include directory" FORCE)
    set(RADRAY_DXC_RUNTIME_TARGET "RadRayDXC::Compiler" CACHE INTERNAL
        "DXC runtime target" FORCE)
    set(RADRAY_DXC_PACKAGE_MODE "fork" CACHE INTERNAL "Shader compiler package mode" FORCE)
    message(STATUS "RadRayDXC capability: RadRay fork package at ${_DXC_ROOT}; extension ABI is required")

    # 所有 radray 二进制经 radray_set_build_path 落在同一个 ${RADRAY_BUILD_PATH}/$<CONFIG>,
    # client 以裸名 "dxcompiler" 动态加载, 只要 DLL 与 exe 同目录即可找到。因此运行库
    # 部署只需要集中一处, 不需要每个消费 target 各自 POST_BUILD 拷贝。
    if (NOT TARGET radray_dxc_runtime_deploy)
        add_custom_target(radray_dxc_runtime_deploy)
        add_custom_command(TARGET radray_dxc_runtime_deploy POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${RADRAY_BUILD_PATH}/$<CONFIG>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:${RADRAY_DXC_RUNTIME_TARGET}>"
                "${RADRAY_BUILD_PATH}/$<CONFIG>/"
            COMMENT "Deploy RadRay DXC compiler runtime to build output")
    endif()
endif()

if (RADRAY_ENABLE_ZLIB)
    set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZLIB_INSTALL OFF CACHE BOOL "" FORCE)
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory(${RADRAY_THIRDPARTY_ROOT}/zlib)
    radray_optimize_flags_library(zlibstatic)
    radray_set_build_path(zlibstatic)
    if (TARGET zlibstatic AND NOT TARGET ZLIB::ZLIB)
        add_library(ZLIB::ZLIB ALIAS zlibstatic)
    endif()
    set(ZLIB_FOUND TRUE CACHE BOOL "ZLIB found" FORCE)
    set(ZLIB_INCLUDE_DIR "${RADRAY_THIRDPARTY_ROOT}/zlib" CACHE PATH "ZLIB include dir" FORCE)
    set(ZLIB_LIBRARY zlibstatic CACHE FILEPATH "ZLIB library" FORCE)
    set(ZLIB_LIBRARIES zlibstatic CACHE FILEPATH "ZLIB libraries" FORCE)
endif()

if (RADRAY_ENABLE_LIBPNG)
    set(PNG_TESTS OFF CACHE BOOL "" FORCE)
    set(PNG_TOOLS OFF CACHE BOOL "" FORCE)
    set(PNG_SHARED OFF CACHE BOOL "" FORCE)
    set(PNG_FRAMEWORK OFF CACHE BOOL "" FORCE)
    set(ZLIB_ROOT "${RADRAY_THIRDPARTY_ROOT}/zlib" CACHE PATH "ZLIB root" FORCE)
    add_subdirectory(${RADRAY_THIRDPARTY_ROOT}/libpng)
    radray_optimize_flags_library(png_static)
    radray_set_build_path(png_static)
    if (TARGET png_static AND NOT TARGET PNG::PNG)
        add_library(PNG::PNG ALIAS png_static)
    endif()
    set(PNG_FOUND TRUE CACHE BOOL "PNG found" FORCE)
    set(PNG_INCLUDE_DIR "${RADRAY_THIRDPARTY_ROOT}/libpng" "${CMAKE_BINARY_DIR}/third_party/libpng" CACHE PATH "PNG include dir" FORCE)
    set(PNG_INCLUDE_DIRS "${PNG_INCLUDE_DIR}" CACHE PATH "PNG include dirs" FORCE)
    set(PNG_LIBRARY png_static CACHE FILEPATH "PNG library" FORCE)
    set(PNG_LIBRARIES png_static CACHE FILEPATH "PNG libraries" FORCE)
    set(PNG_PNG_INCLUDE_DIR "${PNG_INCLUDE_DIR}" CACHE PATH "PNG include dir" FORCE)
endif()

if (RADRAY_ENABLE_LIBJPEG)
    set(RADRAY_LIBJPEG_INSTALL_DIR "${CMAKE_BINARY_DIR}/third_party/libjpeg-turbo-install")
    set(RADRAY_LIBJPEG_BUILD_DIR "${CMAKE_BINARY_DIR}/third_party/libjpeg-turbo-build")
    if (WIN32)
        set(RADRAY_LIBJPEG_LIB "${RADRAY_LIBJPEG_INSTALL_DIR}/lib/jpeg-static.lib")
    else()
        set(RADRAY_LIBJPEG_LIB "${RADRAY_LIBJPEG_INSTALL_DIR}/lib/libjpeg.a")
    endif()
    ExternalProject_Add(radray_libjpeg_turbo_ep
        SOURCE_DIR "${RADRAY_THIRDPARTY_ROOT}/libjpeg-turbo"
        BINARY_DIR "${RADRAY_LIBJPEG_BUILD_DIR}"
        INSTALL_DIR "${RADRAY_LIBJPEG_INSTALL_DIR}"
        CMAKE_ARGS
            "-DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>"
            "-DCMAKE_BUILD_TYPE=$<CONFIG>"
            "-DWITH_CRT_DLL=ON"
            "-DENABLE_SHARED=OFF"
            "-DENABLE_STATIC=ON"
            "-DWITH_JPEG8=ON"
            "-DWITH_TURBOJPEG=OFF"
            "-DWITH_TOOLS=OFF"
            "-DWITH_TESTS=OFF"
            "-DWITH_FUZZ=OFF"
            "-DWITH_JAVA=OFF"
            "-DWITH_DOCS=OFF"
        BUILD_BYPRODUCTS "${RADRAY_LIBJPEG_LIB}")
    file(MAKE_DIRECTORY "${RADRAY_LIBJPEG_INSTALL_DIR}/include")
    add_library(jpeg-static STATIC IMPORTED GLOBAL)
    add_dependencies(jpeg-static radray_libjpeg_turbo_ep)
    set_target_properties(jpeg-static PROPERTIES
        IMPORTED_LOCATION "${RADRAY_LIBJPEG_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${RADRAY_LIBJPEG_INSTALL_DIR}/include")
endif()

if (RADRAY_ENABLE_VULKAN)
    set(PROJECT_IS_TOP_LEVEL OFF CACHE BOOL "" FORCE)
    add_subdirectory(${RADRAY_THIRDPARTY_ROOT}/Vulkan-Headers)
    if (DEFINED vulkan-headers_SOURCE_DIR)
        set(VULKAN_HEADERS_INSTALL_DIR "${vulkan-headers_SOURCE_DIR}" CACHE PATH "" FORCE)
    endif()
    set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "" FORCE)
    set(VOLK_HEADERS_ONLY ON CACHE BOOL "" FORCE)
    add_subdirectory(${RADRAY_THIRDPARTY_ROOT}/volk)
    add_subdirectory(${RADRAY_THIRDPARTY_ROOT}/VulkanMemoryAllocator)
endif()

if (RADRAY_ENABLE_D3D12 OR (WIN32 AND RADRAY_BUILD_SHADER_COMPILER))
    set(DXHEADERS_BUILD_TEST OFF CACHE BOOL "" FORCE)
    set(DXHEADERS_INSTALL OFF CACHE BOOL "" FORCE)
    set(DXHEADERS_BUILD_GOOGLE_TEST OFF CACHE BOOL "" FORCE)
    add_subdirectory(${RADRAY_THIRDPARTY_ROOT}/DirectX-Headers)
endif()

if (RADRAY_ENABLE_D3D12)
    add_subdirectory(${RADRAY_THIRDPARTY_ROOT}/D3D12MemoryAllocator)
    target_compile_options(D3D12MemoryAllocator PRIVATE 
        $<$<AND:$<CXX_COMPILER_ID:Clang>,$<BOOL:${MSVC}>>:-Wno-unused-variable>
        $<$<AND:$<CXX_COMPILER_ID:Clang>,$<BOOL:${MSVC}>>:-Wno-switch>
        $<$<AND:$<CXX_COMPILER_ID:Clang>,$<BOOL:${MSVC}>>:-Wno-unused-const-variable>
        $<$<AND:$<CXX_COMPILER_ID:Clang>,$<BOOL:${MSVC}>>:-Wno-unused-function>)
    radray_optimize_flags_library(D3D12MemoryAllocator)
    radray_set_build_path(D3D12MemoryAllocator)
endif()

if (RADRAY_BUILD_TESTS)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    add_subdirectory(${RADRAY_THIRDPARTY_ROOT}/googletest)
    radray_optimize_flags_library(gtest)
    radray_set_build_path(gtest)
    radray_optimize_flags_library(gtest_main)
    radray_set_build_path(gtest_main)
    # Workaround for clang 21+ warning: implicit char8_t -> char32_t conversion
    # https://github.com/google/googletest/issues/4762
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(gtest PRIVATE -Wno-character-conversion)
        target_compile_options(gtest_main PRIVATE -Wno-character-conversion)
    endif()
endif()
if (RADRAY_BUILD_BENCHMARKS)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_INSTALL_TOOLS OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_USE_BUNDLED_GTEST OFF CACHE BOOL "" FORCE)
    add_subdirectory(${RADRAY_THIRDPARTY_ROOT}/googlebenchmark)
    radray_optimize_flags_library(benchmark)
    radray_set_build_path(benchmark)
endif()

# 添加工程子目录
if (RADRAY_BUILD_TESTS)
    enable_testing()
    include(GoogleTest)
endif()
add_subdirectory(modules)
if (RADRAY_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
if (RADRAY_BUILD_SHADER_TOOLS)
    add_subdirectory(tools)
endif()
if (RADRAY_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
