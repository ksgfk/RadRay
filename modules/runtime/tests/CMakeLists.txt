radray_add_test(test_vertical_slice SOURCES test_vertical_slice.cpp LINK_LIBS radrayruntime DISCOVER_ARGS PROPERTIES ENVIRONMENT "RADRAY_PROJECT_DIR=${CMAKE_SOURCE_DIR}")

# ShaderAsset / ShaderPassProgram: 加载期要建真实 PipelineLayout, 故需要 device,
# 无设备时用例自行 GTEST_SKIP。
radray_add_test(test_shader_program SOURCES test_shader_program.cpp LINK_LIBS radrayruntime DISCOVER_ARGS PROPERTIES ENVIRONMENT "RADRAY_PROJECT_DIR=${CMAKE_SOURCE_DIR}")

# manifest -> RHI 描述的打包测试, 不需要 device。
radray_add_test(test_shader_layout_binding SOURCES test_shader_layout_binding.cpp LINK_LIBS radrayruntime)

# PipelineStateCache: 建真实 PSO 以验证缓存 key 的每一维, 故需要 device 与 DXC,
# 二者缺一时用例自行 GTEST_SKIP。
radray_add_test(test_pipeline_state_cache SOURCES test_pipeline_state_cache.cpp LINK_LIBS radrayruntime DISCOVER_ARGS PROPERTIES ENVIRONMENT "RADRAY_PROJECT_DIR=${CMAKE_SOURCE_DIR}")

# 把仓库根编进二进制, 作为环境变量缺失时的兜底。
#
# 【为何需要】: 上面的 ENVIRONMENT 只在 ctest 启动时注入, 直接跑 exe (调试器、
# --gtest_filter 手动复现) 时环境是空的, 测试会读到空路径。既不能退化成相对路径
# "shaderlib" (那样只在 cwd 恰好是仓库根时通过, 把配置错误伪装成通过), 也不该
# 让这些用例只能经 ctest 运行。仓库根是配置期已知的常量, 编进去即可。
#
# 环境变量优先: 它使产物可以被搬到别处、指向另一份源码树运行。
#
# 垂直切片: shader manifest -> PSO -> 真实 GPU 绘制 -> 像素读回。
# 需要 shaderlib 与 error_pass.shader.json, 故同样依赖 RADRAY_PROJECT_DIR。
# 无设备/无 DXC 时用例自行 GTEST_SKIP, 不会让无 GPU 的 CI 变红。
foreach(_radray_shader_test IN ITEMS test_vertical_slice test_shader_program test_pipeline_state_cache)
    target_compile_definitions(${_radray_shader_test} PRIVATE
        RADRAY_PROJECT_DIR_DEFAULT="${CMAKE_SOURCE_DIR}")
endforeach()
unset(_radray_shader_test)
