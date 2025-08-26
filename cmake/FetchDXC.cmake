#[=======================================================================[.rst:
FetchDXC.cmake
----------------

下载并引入预编译的 DirectX Shader Compiler (dxc) 二进制包（无 CMake 工程），
创建 imported target:  dxc::dxcompiler

用法（顶层 CMakeLists.txt）:

  set(RADRAY_ENABLE_DXC ON)
  include(cmake/FetchDXC.cmake)
  target_link_libraries(radraycore PRIVATE $<BOOL:${RADRAY_ENABLE_DXC}:dxc::dxcompiler>)

可配置变量:
    RADRAY_DXC_VERSION     默认 1.8.2505.1
    RADRAY_DXC_URL_HASH    可选，提供 EXPECTED_HASH（如 SHA256=<hash>）进行完整性校验
    RADRAY_DXC_COPY_RUNTIME 若 ON (默认 ON / Windows)，配置阶段记录需要复制的运行时 DLL

目录结构假设：
  Windows: bin/ (dxcompiler.dll dxil.dll)  lib/ (dxcompiler.lib) include/
  Linux:   lib/libdxcompiler.so            include/
  macOS:   lib/libdxcompiler.dylib         include/

注意: 使用 FetchContent_Populate 而不是 MakeAvailable，因为这是纯二进制包没有 CMakeLists.txt。
#]=======================================================================]

if (TARGET dxc::dxcompiler)
    # 已创建
    return()
endif()

set(RADRAY_DXC_VERSION "1.8.2505.1" CACHE STRING "DXC prebuilt package version")
set(RADRAY_DXC_COPY_RUNTIME_DEFAULT OFF)
if (WIN32)
    set(RADRAY_DXC_COPY_RUNTIME_DEFAULT ON)
endif()
set(RADRAY_DXC_COPY_RUNTIME ${RADRAY_DXC_COPY_RUNTIME_DEFAULT} CACHE BOOL "Copy DXC runtime DLLs (Windows / other OS shared lib)")

# 归档名选择（固定 URL 组成）
if (WIN32)
    set(_DXC_ARCHIVE "dxc-windows-x64.zip")
elseif (APPLE)
    if (CMAKE_SYSTEM_PROCESSOR MATCHES "[Aa][Rr][Mm]64|aarch64")
        set(_DXC_ARCHIVE "dxc-macos-arm64.tar.gz")
    else()
        message(FATAL_ERROR "未定义的 macOS 架构: ${CMAKE_SYSTEM_PROCESSOR}")
    endif()
elseif (UNIX)
    set(_DXC_ARCHIVE "dxc-linux-x64.tar.gz")
else()
    message(FATAL_ERROR "不支持的平台用于 DXC 下载")
endif()
set(_DXC_URL "https://github.com/ksgfk/dxc-autobuild/releases/download/v${RADRAY_DXC_VERSION}/${_DXC_ARCHIVE}")

# 平台专用哈希可选：用户可通过以下变量之一提供（优先级低于显式 RADRAY_DXC_URL_HASH）
#   RADRAY_DXC_HASH_WINDOWS_X64
#   RADRAY_DXC_HASH_LINUX_X64
#   RADRAY_DXC_HASH_MACOS_ARM64
# 开关：RADRAY_DXC_ENFORCE_HASH=ON 时若未能确定哈希则报错。
set(RADRAY_DXC_ENFORCE_HASH ON CACHE BOOL "Require a known SHA256 hash for DXC archive")

if (NOT RADRAY_DXC_URL_HASH)
    if (WIN32 AND DEFINED RADRAY_DXC_HASH_WINDOWS_X64)
        set(RADRAY_DXC_URL_HASH "${RADRAY_DXC_HASH_WINDOWS_X64}")
    elseif (UNIX AND NOT APPLE AND DEFINED RADRAY_DXC_HASH_LINUX_X64)
        set(RADRAY_DXC_URL_HASH "${RADRAY_DXC_HASH_LINUX_X64}")
    elseif (APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "[Aa][Rr][Mm]64|aarch64" AND DEFINED RADRAY_DXC_HASH_MACOS_ARM64)
        set(RADRAY_DXC_URL_HASH "${RADRAY_DXC_HASH_MACOS_ARM64}")
    endif()
endif()

if (RADRAY_DXC_ENFORCE_HASH AND NOT RADRAY_DXC_URL_HASH)
    message(FATAL_ERROR "启用了 RADRAY_DXC_ENFORCE_HASH 但未提供平台对应的哈希值。请设置 RADRAY_DXC_URL_HASH 或平台变量 (RADRAY_DXC_HASH_WINDOWS_X64 / RADRAY_DXC_HASH_LINUX_X64 / RADRAY_DXC_HASH_MACOS_ARM64)。")
endif()

# 下载与解压（手动实现，避免 FetchContent_Populate 警告）
set(_DXC_WORK_ROOT "${CMAKE_BINARY_DIR}/_dxc")
file(MAKE_DIRECTORY "${_DXC_WORK_ROOT}")
set(_DXC_VERSION_ROOT "${_DXC_WORK_ROOT}/v${RADRAY_DXC_VERSION}")
set(_DXC_ARCHIVE_PATH "${_DXC_VERSION_ROOT}/${_DXC_ARCHIVE}")
set(_DXC_EXTRACT_DIR "${_DXC_VERSION_ROOT}/extracted")
set(_DXC_STAMP "${_DXC_VERSION_ROOT}/.done")

if (NOT EXISTS "${_DXC_STAMP}")
    message(STATUS "Downloading DXC: ${_DXC_URL}")
    file(MAKE_DIRECTORY "${_DXC_VERSION_ROOT}")
    if (RADRAY_DXC_URL_HASH)
        file(DOWNLOAD "${_DXC_URL}" "${_DXC_ARCHIVE_PATH}" SHOW_PROGRESS STATUS _dxc_status TLS_VERIFY ON EXPECTED_HASH ${RADRAY_DXC_URL_HASH})
    else()
        file(DOWNLOAD "${_DXC_URL}" "${_DXC_ARCHIVE_PATH}" SHOW_PROGRESS STATUS _dxc_status TLS_VERIFY ON)
    endif()
    list(GET _dxc_status 0 _dxc_code)
    if (NOT _dxc_code EQUAL 0)
        list(GET _dxc_status 1 _dxc_msg)
        message(FATAL_ERROR "DXC 下载失败: ${_dxc_msg}")
    endif()
    message(STATUS "Extracting DXC to ${_DXC_EXTRACT_DIR}")
    file(REMOVE_RECURSE "${_DXC_EXTRACT_DIR}")
    file(MAKE_DIRECTORY "${_DXC_EXTRACT_DIR}")
    file(ARCHIVE_EXTRACT INPUT "${_DXC_ARCHIVE_PATH}" DESTINATION "${_DXC_EXTRACT_DIR}")
    file(WRITE "${_DXC_STAMP}" "")
endif()

set(_DXC_ROOT "${_DXC_EXTRACT_DIR}")
set(_DXC_INCLUDE_DIR "${_DXC_ROOT}/include")

if (WIN32)
    set(_DXC_BIN_DIR "${_DXC_ROOT}/bin")
    set(_DXC_LIB_DIR "${_DXC_ROOT}/lib")
    set(_DXC_DLL      "${_DXC_BIN_DIR}/dxcompiler.dll")
    set(_DXC_IMPLIB   "${_DXC_LIB_DIR}/dxcompiler.lib")
    # 可选的 dxil 导入库（有些发行包不提供，仅提供 dxil.dll 作为运行时）
    set(_DXC_DXIL_IMPLIB "${_DXC_LIB_DIR}/dxil.lib")
    set(_DXC_DXIL_DLL    "${_DXC_BIN_DIR}/dxil.dll")
    if (NOT EXISTS "${_DXC_IMPLIB}" OR NOT EXISTS "${_DXC_DLL}")
        message(FATAL_ERROR "DXC 预编译包缺少 dxcompiler.dll 或 dxcompiler.lib: ${_DXC_ROOT}")
    endif()
    add_library(dxc::dxcompiler SHARED IMPORTED)
    set_target_properties(dxc::dxcompiler PROPERTIES
        IMPORTED_IMPLIB "${_DXC_IMPLIB}"
        IMPORTED_LOCATION "${_DXC_DLL}"
        INTERFACE_INCLUDE_DIRECTORIES "${_DXC_INCLUDE_DIR}"
    )
    # 若存在 dxil.lib 则也链接（这样可以在链接期解析符号）；否则仅在运行时加载 dxil.dll
    if (EXISTS "${_DXC_DXIL_IMPLIB}")
        set(RADRAY_DXC_HAVE_DXIL_LIB ON CACHE INTERNAL "DXIL import library available")
        set_property(TARGET dxc::dxcompiler APPEND PROPERTY INTERFACE_LINK_LIBRARIES "${_DXC_DXIL_IMPLIB}")
    else()
        set(RADRAY_DXC_HAVE_DXIL_LIB OFF CACHE INTERNAL "DXIL import library available")
    endif()
    if (RADRAY_DXC_COPY_RUNTIME)
        # 复制 dxcompiler.dll 以及可用的 dxil.dll
        if (EXISTS "${_DXC_DXIL_DLL}")
            list(APPEND _DXC_RUNTIME_LIST "${_DXC_DXIL_DLL}")
        endif()
        set(RADRAY_DXC_RUNTIME_DLLS "${_DXC_DLL};${_DXC_RUNTIME_LIST}" CACHE INTERNAL "DXC runtime binaries")
    endif()
elseif (APPLE)
    set(_DXC_LIB "${_DXC_ROOT}/lib/libdxcompiler.dylib")
    set(_DXC_DXIL_LIB "${_DXC_ROOT}/lib/libdxil.dylib")
    if (NOT EXISTS "${_DXC_LIB}")
        message(FATAL_ERROR "未找到 libdxcompiler.dylib: ${_DXC_LIB}")
    endif()
    add_library(dxc::dxcompiler SHARED IMPORTED)
    set_target_properties(dxc::dxcompiler PROPERTIES
        IMPORTED_LOCATION "${_DXC_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${_DXC_INCLUDE_DIR}"
    )
    if (EXISTS "${_DXC_DXIL_LIB}")
        set(RADRAY_DXC_HAVE_DXIL_LIB ON CACHE INTERNAL "DXIL shared library available (macOS)")
        set_property(TARGET dxc::dxcompiler APPEND PROPERTY INTERFACE_LINK_LIBRARIES "${_DXC_DXIL_LIB}")
    else()
        set(RADRAY_DXC_HAVE_DXIL_LIB OFF CACHE INTERNAL "DXIL shared library available (macOS)")
    endif()
    if (RADRAY_DXC_COPY_RUNTIME)
        if (EXISTS "${_DXC_DXIL_LIB}")
            set(RADRAY_DXC_RUNTIME_DLLS "${_DXC_DXIL_LIB}" CACHE INTERNAL "DXC runtime binaries (macOS)")
        endif()
    endif()
else()
    # Linux
    set(_DXC_LIB "${_DXC_ROOT}/lib/libdxcompiler.so")
    set(_DXC_DXIL_LIB "${_DXC_ROOT}/lib/libdxil.so")
    if (NOT EXISTS "${_DXC_LIB}")
        message(FATAL_ERROR "未找到 libdxcompiler.so: ${_DXC_LIB}")
    endif()
    add_library(dxc::dxcompiler SHARED IMPORTED)
    set_target_properties(dxc::dxcompiler PROPERTIES
        IMPORTED_LOCATION "${_DXC_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${_DXC_INCLUDE_DIR}"
    )
    if (EXISTS "${_DXC_DXIL_LIB}")
        set(RADRAY_DXC_HAVE_DXIL_LIB ON CACHE INTERNAL "DXIL shared library available (Linux)")
        set_property(TARGET dxc::dxcompiler APPEND PROPERTY INTERFACE_LINK_LIBRARIES "${_DXC_DXIL_LIB}")
    else()
        set(RADRAY_DXC_HAVE_DXIL_LIB OFF CACHE INTERNAL "DXIL shared library available (Linux)")
    endif()
    if (RADRAY_DXC_COPY_RUNTIME)
        if (EXISTS "${_DXC_DXIL_LIB}")
            set(RADRAY_DXC_RUNTIME_DLLS "${_DXC_DXIL_LIB}" CACHE INTERNAL "DXC runtime binaries (Linux)")
        endif()
    endif()
endif()

mark_as_advanced(RADRAY_DXC_VERSION RADRAY_DXC_URL_HASH RADRAY_DXC_COPY_RUNTIME)
mark_as_advanced(RADRAY_DXC_ENFORCE_HASH RADRAY_DXC_HASH_WINDOWS_X64 RADRAY_DXC_HASH_LINUX_X64 RADRAY_DXC_HASH_MACOS_ARM64)
