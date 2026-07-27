radray_add_test(test_shader_asset SOURCES test_shader_asset.cpp LINK_LIBS radrayruntime DISCOVER_ARGS PROPERTIES ENVIRONMENT "RADRAY_PROJECT_DIR=${CMAKE_SOURCE_DIR}")
# 模板生成用仓库里真实的 shaderlib 做输入, 故同样需要 RADRAY_PROJECT_DIR。
radray_add_test(test_shader_asset_template SOURCES test_shader_asset_template.cpp LINK_LIBS radrayruntime DISCOVER_ARGS PROPERTIES ENVIRONMENT "RADRAY_PROJECT_DIR=${CMAKE_SOURCE_DIR}")

# 把仓库根编进二进制, 作为环境变量缺失时的兜底。
#
# 【为何需要】: 上面的 ENVIRONMENT 只在 ctest 启动时注入, 直接跑 exe (调试器、
# --gtest_filter 手动复现) 时环境是空的, 测试会读到空路径。既不能退化成相对路径
# "shaderlib" (那样只在 cwd 恰好是仓库根时通过, 把配置错误伪装成通过), 也不该
# 让这些用例只能经 ctest 运行。仓库根是配置期已知的常量, 编进去即可。
#
# 环境变量优先: 它使产物可以被搬到别处、指向另一份源码树运行。
foreach(_radray_shader_test IN ITEMS test_shader_asset test_shader_asset_template)
    target_compile_definitions(${_radray_shader_test} PRIVATE
        RADRAY_PROJECT_DIR_DEFAULT="${CMAKE_SOURCE_DIR}")
endforeach()
unset(_radray_shader_test)
