# Repository Guidelines
## Project Structure & Module Organization
- `modules/` contains core code split into `core`, `window`, `render`, and `imgui`.
- `examples/` holds runnable demos; `benchmarks/` holds performance targets (enabled in release builds).
- `tools/` contains utility binaries and helper tooling.
- `assets/` and `shaderlib/` store runtime assets and shader sources.
- `third_party/` and `SDKs/` are dependency trees populated by setup scripts.
## Build
- Config `cmake --preset win-x64-debug-clangcl`/`cmake --preset win-x64-debug`
- Compile `cmake --build build_debug --parallel 24`
## Coding Style & Naming Conventions
- Language baseline: C++20 (`CMAKE_CXX_STANDARD 20`), C11 for C sources.
- STL 容器优先使用`radray`命名空间下的, 例如`string`/`vector`/`unordered_map`, 详情看 `radray/types.h` 头文件
- 接口的可空指针使用 `Nullable<T>`, 裸指针代表不可空
- DEBUG模式用宏`RADRAY_IS_DEBUG`
## Test
- 各个模块的测试用例存入模块文件夹下的`tests`内，例如`core`的在`modules/core/tests/`
- 一般测试在`CMakeLists.txt`内用`radray_add_test`函数，需要特殊环境变量定义`RADRAY_PROJECT_DIR`、`RADRAY_TEST_ENV_DIR`等的用`radray_add_radray_gtest_case`
- CTest `ctest --preset win-x64 -R BuddyAllocatorTest --output-on-failure` 注意-R后面要跟代码里写的gtest test suit名字，而不是cmake里定义的名，因为是GTest
