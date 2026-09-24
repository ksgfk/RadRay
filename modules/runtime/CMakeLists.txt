file(GLOB_RECURSE RADRAY_RUNTIME_INC CONFIGURE_DEPENDS "include/*.h")
file(GLOB_RECURSE RADRAY_RUNTIME_SRC CONFIGURE_DEPENDS "src/*.cpp")

add_library(radrayruntime STATIC ${RADRAY_RUNTIME_SRC} ${RADRAY_RUNTIME_INC})
target_include_directories(radrayruntime PUBLIC include)
target_link_libraries(radrayruntime PUBLIC
    radraycore
    radrayshader
    radrayrender
    radraywindow)
target_link_libraries(radrayruntime PRIVATE cgltf::cgltf)
if (RADRAY_ENABLE_SHADER_JIT)
    target_compile_definitions(radrayruntime PRIVATE RADRAY_ENABLE_SHADER_JIT)
    target_link_libraries(radrayruntime PUBLIC radrayshadercompiler)
endif()
if (WIN32 AND (RADRAY_ENABLE_D3D12 OR RADRAY_ENABLE_VULKAN))
    target_link_libraries(radrayruntime PRIVATE dxgi)
endif()
radray_default_compile_flags(radrayruntime)
radray_optimize_flags_library(radrayruntime)
radray_set_build_path(radrayruntime)
radray_enable_unity_build(radrayruntime)
# PUBLIC：链上 radrayruntime 的目标会继承这份头列表，并各自再编一份预编译头。
# 不放 coroutine.h：它带入的 stdexec 用泛型 lambda 作模板参数，clang-cl 的预编译头
# 会把该 lambda 和翻译单元里的当成不同类型。
# win32_headers.h 不放进来：windows.h 的宏会进入所有翻译单元。
target_precompile_headers(radrayruntime PUBLIC
    <Eigen/Dense>
    <fmt/format.h>
    <fmt/printf.h>
    <fmt/xchar.h>
    <magic_enum/magic_enum.hpp>
    <magic_enum/magic_enum_flags.hpp>
    <sigslot/signal.hpp>
    <radray/types.h>
    <radray/nullable.h>
    <radray/logger.h>
    <radray/enum_flags.h>
    <radray/runtime_type.h>
    <radray/scope_guard.h>
    <radray/hash.h>
    <radray/file.h>
    <radray/text_encoding.h>
    <radray/channel.h>
    <radray/sparse_set.h>
    <radray/intrusive_ptr.h>
    <radray/image_data.h>
    <radray/json.h>
    <radray/basic_math.h>
    <radray/profiler.h>
    <radray/render/rhi.h>)

if (RADRAY_BUILD_TESTS)
    add_subdirectory(tests)
endif()
