# radray_shader_cook: 把 *.shader.json 烘成 AOT 产物 (index.json + <category>/<key>.bin)。
#
# 只在有 DXC (RADRAY_ENABLE_SHADER_JIT) 时构建 —— cook 本质就是提前做 JIT。
# 该守卫由 tools/CMakeLists.txt 施加。

add_executable(radray_shader_cook shader_cook.cpp)
target_link_libraries(radray_shader_cook PRIVATE radrayruntime)
if (RADRAY_ENABLE_MIMALLOC)
    target_link_libraries(radray_shader_cook PRIVATE mimalloc-override)
endif()
radray_default_compile_flags(radray_shader_cook)
radray_optimize_flags_binary(radray_shader_cook)
radray_set_build_path(radray_shader_cook)

# DXC 运行库要在 exe 旁边才能加载。radrayruntime 的 POST_BUILD 已把它放进同一个输出
# 目录, 故此处无需重复部署。
add_dependencies(radray_shader_cook radrayruntime)

# 显式烘焙目标。【不进 ALL】—— 烘焙不是构建的一部分:
# - 开发构建靠 JIT, 产物只会带来"改了 shader 却读到旧 blob"这类多余的失效面;
# - cook 要跑 DXC 编译每个变体, 挂进每次构建等于给所有人加一笔与其工作无关的开销;
# - AOT 是发布/打包的需求, 该由打包流程按需触发, 而不是由"编了一次代码"触发。
#
# 用法: cmake --build <dir> --target radray_cook_shaders
#
# 烘的是输出目录里那份 shaderlib 而非源码树: 运行时 ShaderResolver 用
# GetShaderArtifactDirectory 从 manifest 路径反推产物目录, 而它拿到的 manifest 在
# <exe>/shaderlib 下 (render_system.cpp 的 _shaderIncludeRoot)。烘源码树等于把产物放到
# 运行时不会去看的地方, 还会污染 git 工作区。
add_custom_target(radray_cook_shaders
    COMMAND radray_shader_cook
        --shader-root "$<TARGET_FILE_DIR:radray_shader_cook>/shaderlib"
        --discover
    DEPENDS radray_shader_cook
    COMMENT "Cook shader assets in the build output")
