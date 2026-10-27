cmake_minimum_required(VERSION 3.25)

if (POLICY CMP0077)
    cmake_policy(SET CMP0077 NEW)
endif()

project(RadRay LANGUAGES C CXX)
set(RADRAY_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(RADRAY_BINARY_ROOT "${CMAKE_CURRENT_BINARY_DIR}")

if (APPLE)
    enable_language(OBJC OBJCXX)
endif()

include("${RADRAY_SOURCE_ROOT}/cmake/Options.cmake")
include("${RADRAY_SOURCE_ROOT}/cmake/DependencyCatalog.cmake")
include(GNUInstallDirs)

list(PREPEND CMAKE_MODULE_PATH "${RADRAY_SOURCE_ROOT}/cmake")

# 设置全局编译器选项
if (NOT DEFINED CMAKE_CXX_STANDARD OR CMAKE_CXX_STANDARD LESS 20)
    set(CMAKE_CXX_STANDARD 20)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
# 本项目不使用 C++20 modules, 关闭模块依赖扫描.
# 必须在任何模块 add_subdirectory 之前设置。
# 原因: CMP0155 在 cmake_minimum_required(VERSION 3.25) 下为 OLD, 但 CMake 4.4 的
# Visual Studio 生成器仍会对声明 C++20 的目标写出 ScanSourceForModuleDependencies=true,
# 而 ClangCL 工具链不认识 MSBuild 传入的 /scanDependencies, 导致 .obj 根本不生成,
# 随后 llvm-lib 以 MSB6006 失败. 增量构建会因残留的 .lib 掩盖该错误, 只有干净构建才暴露.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
if (MSVC)
    add_compile_options(/MP)
endif()

# 设置全局构建输出路径
set(RADRAY_BUILD_PATH "${RADRAY_BINARY_ROOT}/_build" CACHE PATH "RadRay build binary path")
if (NOT RADRAY_BUILD_PATH)
    set(RADRAY_BUILD_PATH "${RADRAY_BINARY_ROOT}/_build")
endif()

include("${RADRAY_SOURCE_ROOT}/cmake/Utility.cmake")
include("${RADRAY_SOURCE_ROOT}/cmake/Dependencies.cmake")
radray_configure_dependencies()
if (RADRAY_BUILD_COMPILE_COMMANDS)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

# 添加工程子目录
if (RADRAY_BUILD_TESTS)
    enable_testing()
    include(GoogleTest)
    if (RADRAY_BUILD_SHADER OR RADRAY_BUILD_RENDER)
        include("${RADRAY_SOURCE_ROOT}/cmake/TestShaders.cmake")
    endif()
endif()

add_subdirectory(modules)
if (RADRAY_BUILD_SHADER_TOOLS)
    add_subdirectory(tools)
endif()
if (RADRAY_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if (RADRAY_BUILD_COMPILE_COMMANDS)
    radray_add_compile_commands_target()
endif()

include("${RADRAY_SOURCE_ROOT}/cmake/Install.cmake")
