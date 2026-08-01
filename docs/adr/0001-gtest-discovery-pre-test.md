# ADR-0001 gtest 测试发现钉死 PRE_TEST

状态: 生效
日期: 2026-07
影响: `cmake/Utility.cmake` 的 `radray_add_test`；全部 `modules/*/tests/CMakeLists.txt`

## 背景

`modules/core/tests` 下有 13 个测试目标共处一个目录。构建时随机出现
`string sub-command JSON failed parsing json string` 失败，重跑有时又能过。

根因在 CMake 4.4 自带的 `GoogleTest.cmake`：它生成 `gtest_discover_tests_impl(...)` 调用时
**从不传 `TEST_TARGET`**。于是 `GoogleTestAddTests.cmake` 里的

```cmake
string(SHA256 target_hash "${arg_TEST_TARGET}")
```

恒等于空串的哈希 `e3b0c44298...`。那个哈希存在的唯一目的就是防止同目录多个目标争用同一个
`cmake_test_discovery_<hash>.json`，结果被空输入彻底废掉——同目录所有目标共用一个发现文件。

默认的 `POST_BUILD` 发现模式是并行执行的，于是 N 个目标同时读写同一个 json。表现有两种：
构建期随机 JSON 解析失败，或者更坏——测试被静默注册到错误的可执行文件上，而 ctest 仍然报绿。

## 决策

`radray_add_test` 内部无条件传 `DISCOVERY_MODE PRE_TEST`，调用方不得覆盖。
`PRE_TEST` 在 ctest 启动阶段串行执行发现，不存在这场竞争。

## 放弃的方案及代价

- **保留 POST_BUILD，给每个测试目标单独建子目录**。能绕开哈希冲突，但把 13 个
  `CMakeLists.txt` 变成 13 个目录，且这个约束完全不可见——后来的人在同目录加第二个测试就会复现。
- **自己给 `gtest_discover_tests` 传 `TEST_TARGET`**。它不是公开参数，`gtest_discover_tests`
  的签名里没有这一项，传了会被 `cmake_parse_arguments` 丢进 UNPARSED。
- **打补丁改 `third_party` 或 CMake 自带模块**。`third_party/` 是脚本填充的只读树，
  CMake 模块更不属于本仓库。

`PRE_TEST` 的代价是发现被推迟到 `ctest` 运行时，因此**只构建不跑 ctest 时看不到测试列表**。
这个代价可接受。

## 必须保持为真

- `cmake/Utility.cmake` 里 `gtest_discover_tests` 的两处调用都带 `DISCOVERY_MODE PRE_TEST`。
- 判断测试覆盖不能只看 `ctest -N` 的计数。要双向比对每个 exe 的 `--gtest_list_tests`
  输出与 ctest 注册列表——注册到错误 exe 的情况下计数仍然是对的。
