file(GLOB_RECURSE RADRAY_SHADER_INC CONFIGURE_DEPENDS "include/*.h")
file(GLOB RADRAY_SHADER_SRC CONFIGURE_DEPENDS "src/*.cpp")
# dxc.cpp / spvc.cpp 是可选后端的唯一使用者, 未开启时必须排除而不是靠 #ifdef 空编译,
# 否则 GLOB 会把它们塞进编译列表并引用不存在的第三方头。
list(REMOVE_ITEM RADRAY_SHADER_SRC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dxc.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/spvc.cpp")
if (RADRAY_ENABLE_DXC)
    list(APPEND RADRAY_SHADER_SRC "src/dxc.cpp")
endif()
if (RADRAY_ENABLE_SPIRV_CROSS)
    list(APPEND RADRAY_SHADER_SRC "src/spvc.cpp")
endif()

add_library(radrayshader STATIC ${RADRAY_SHADER_SRC} ${RADRAY_SHADER_INC})
target_include_directories(radrayshader PUBLIC include)
target_link_libraries(radrayshader PUBLIC radraycore)
# DXC 与 spirv-cross 是实现细节, 用 PRIVATE 链接: 使用者只看到 dxc.h 里的
# radray 类型, 不会继承第三方头的包含路径。
target_link_libraries(radrayshader PRIVATE
    $<$<BOOL:${RADRAY_ENABLE_DXC}>:dxc::dxcompiler>
    $<$<AND:$<BOOL:${RADRAY_ENABLE_DXC}>,$<PLATFORM_ID:Windows>>:Microsoft::DirectX-Headers>
    $<$<BOOL:${RADRAY_ENABLE_SPIRV_CROSS}>:spirv-cross-core>
    $<$<BOOL:${RADRAY_ENABLE_SPIRV_CROSS}>:spirv-cross-glsl>
    $<$<BOOL:${RADRAY_ENABLE_SPIRV_CROSS}>:spirv-cross-msl>
    $<$<BOOL:${RADRAY_ENABLE_SPIRV_CROSS}>:spirv-cross-util>)
# 【必须 PUBLIC】dxc.h / spvc.h 的声明本身被这两个宏守卫, 使用者 (radrayrender、
# 两个 CLI、runtime 格式层) 必须看到相同的宏值, 否则会静默地看不到函数声明。
# RADRAY_ENABLE_SHADER_JIT 同理: 它影响 shader_manifest.h 暴露的 JIT 接口。
target_compile_definitions(radrayshader PUBLIC
    $<$<BOOL:${RADRAY_ENABLE_DXC}>:RADRAY_ENABLE_DXC>
    $<$<BOOL:${RADRAY_ENABLE_SPIRV_CROSS}>:RADRAY_ENABLE_SPIRV_CROSS>
    $<$<BOOL:${RADRAY_ENABLE_SHADER_JIT}>:RADRAY_ENABLE_SHADER_JIT>)
# DXC 版本进 toolchain hash: 换编译器必须让已有 AOT 产物与磁盘缓存全部失效。
#
# 【必须无条件注入】: 该版本号是 project_manifest.json 里钉住的 SDK 版本, 与本次构建
# 是否编入 JIT 无关。若只在 RADRAY_ENABLE_SHADER_JIT 下注入, 则关 JIT 的发布包算出的
# toolchain hash 与 cook 机 (开 JIT) 不同, "只带产物、不带 DXC" 的场景会 100% 加载失败。
if (NOT DEFINED RADRAY_SDK_dxc_VERSION)
    message(FATAL_ERROR "RADRAY_SDK_dxc_VERSION is required for the shader toolchain hash")
endif()
target_compile_definitions(radrayshader PRIVATE
    RADRAY_DXC_VERSION="${RADRAY_SDK_dxc_VERSION}")
radray_default_compile_flags(radrayshader)
radray_optimize_flags_library(radrayshader)
radray_set_build_path(radrayshader)

if (RADRAY_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if (RADRAY_ENABLE_SHADER_JIT)
    # 先删再拷: copy_directory 不会移除源码树里已删掉的文件, 残留的旧头文件会让
    # 重命名后的 #include 悄悄解析到过期副本, 排查起来极其费时。
    #
    # 【落点靠 radray_set_build_path 收敛】radrayshader 是静态库, 但上面的
    # radray_set_build_path 把它的 TARGET_FILE_DIR 设成与所有 exe 相同的
    # _build/<Config>/, 所以这里部署的 shaderlib 与 DXC dll 就在可执行文件旁边。
    # 若改动 radray_set_build_path, 必须重新实测本目录, 否则只在运行期暴露。
    set(_RADRAY_SHADER_JIT_DEPLOY_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:radrayshader>/shaderlib"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/shaderlib" "$<TARGET_FILE_DIR:radrayshader>/shaderlib")
    foreach(_dll IN LISTS RADRAY_DXC_RUNTIME_DLLS)
        list(APPEND _RADRAY_SHADER_JIT_DEPLOY_COMMANDS
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_dll}" "$<TARGET_FILE_DIR:radrayshader>")
    endforeach()
    add_custom_command(TARGET radrayshader POST_BUILD
        ${_RADRAY_SHADER_JIT_DEPLOY_COMMANDS}
        COMMENT "Deploy shader sources and DXC runtime for shader JIT")
    unset(_RADRAY_SHADER_JIT_DEPLOY_COMMANDS)
endif()
