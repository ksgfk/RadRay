# radray_shader_gen: 从 HLSL 反射生成 *.shader.json 起始模板的命令行工具。
#
# 只在有 DXC (RADRAY_ENABLE_SHADER_JIT) 时构建 —— 生成模板本质上就是编译一遍再读反射。
# 该守卫由 tools/CMakeLists.txt 施加。

add_executable(radray_shader_gen shader_gen.cpp)
# 【只链 radrayshader】同 shader_cook: 反射生成不需要 GPU 设备, 链 runtime 会白拖
# 整套图形后端。详见 tools/shader_cook/CMakeLists.txt 的说明。
target_link_libraries(radray_shader_gen PRIVATE radrayshader)
if (RADRAY_ENABLE_MIMALLOC)
    target_link_libraries(radray_shader_gen PRIVATE mimalloc-override)
endif()
radray_default_compile_flags(radray_shader_gen)
radray_optimize_flags_binary(radray_shader_gen)
radray_set_build_path(radray_shader_gen)

# 生成器要读 shaderlib 才能编译, DXC 运行库要在 exe 旁边才能加载。
# radrayshader 的 POST_BUILD 已把两者放进同一个输出目录, 故此处无需重复部署。
add_dependencies(radray_shader_gen radrayshader)
