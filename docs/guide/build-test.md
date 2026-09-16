> - 适用: 恢复依赖、配置与构建 RadRay、运行测试或生成编译数据库
> - 权威: 本文说明仓库构建操作；选项与预设以锚点中的 CMake 文件为准
> - 锚点: `CMakeLists.txt`, `CMakePresets.json`, `cmake/Utility.cmake`, `project_manifest.json`, `tools/fetch_third_party.py`, `tools/fetch_sdks.py`, `tools/win_gen_compile_commands.py`, `modules/*/tests/CMakeLists.txt`

# 构建与测试

命令在仓库根执行。Windows Ninja 预设需要已配置 x64 C++ 工具链的终端，以及 CMake、Ninja、
Python 和 Git。现有构建目录的 generator 以其 `CMakeCache.txt` 为准，不直接用另一 generator
覆盖；需要切换时用 `-B` 指定独立构建目录。

## 恢复与构建

```powershell
python tools/fetch_third_party.py restore
python tools/fetch_sdks.py restore
cmake --preset win-x64-debug
cmake --build build_debug --config Debug --parallel 24
```

恢复脚本读取 `project_manifest.json`。第三方源码进入 `third_party/`，RadRay DXC package
进入 `SDKs/radray_dxc/extracted`；不手改这些目录。shader compiler 开启时需要匹配的 fork
package，stock DXC 不能替代。包的 ABI 与部署边界见 [Shader pipeline](../architecture/shader-pipeline.md)。

`win-x64-debug` / `win-x64-release` 使用 Ninja，分别输出到 `build_debug` / `build_release`。
`win-x64-*-clangcl` 使用 Visual Studio 18 2026 的 ClangCL toolset；它们与 Ninja 预设默认共用
同名目录，切换时应另给 `-B`。例如新建独立 ClangCL 构建：

```powershell
cmake --preset win-x64-debug-clangcl -B build_clangcl
cmake --build build_clangcl --config Debug --parallel 24
```

二进制经 `radray_set_build_path` 放在 `<build-dir>/_build/<Config>/`，可由
`RADRAY_BUILD_PATH` 改写基础路径。macOS 的 Ninja 预设为 `macos-arm64-debug` / `macos-arm64-release`，
是否可用还取决于 manifest 对应平台依赖；不能将 Windows 验收结果当作 macOS 验证。

### Debug 配置的运行时检查

`Debug` 保留 CMake/MSVC 默认的 `/Od /Ob0 /RTC1`，但根 `CMakeLists.txt` 在任何 `add_subdirectory`
之前全局定义两项宏，以换取可用的 Debug 帧率：

| 宏 | 作用 | 保留的检查 |
|---|---|---|
| `_ITERATOR_DEBUG_LEVEL=1`（仅 MSVC） | 关闭 STL 迭代器 owner 追踪与加锁 | 越界、失效迭代器解引用 |

`_ITERATOR_DEBUG_LEVEL` 影响 STL 对象布局，必须在同一二进制的所有 C++ TU 一致，所以在此全局设置，
第三方库随工程编译时自动继承；预编译的 DXC package 与 libjpeg-turbo 是 C 接口，不受影响。
不在单个 target 上改写它。`RelWithDebInfo`/`MinSizeRel` 不受此配置影响。

## 常用配置边界

| 开关 | 默认与依赖 |
|---|---|
| `RADRAY_BUILD_TESTS` | ON |
| `RADRAY_BUILD_BENCHMARKS` | 单配置 Release 或包含 Release 的多配置 generator 默认 ON |
| `RADRAY_BUILD_WINDOW`、`RADRAY_BUILD_RENDER` | ON |
| `RADRAY_BUILD_RUNTIME` | 默认 ON，要求 render 和 window |
| `RADRAY_ENABLE_D3D12` | Windows 且 render 开启时默认 ON |
| `RADRAY_ENABLE_VULKAN` | render 开启时默认 ON |
| `RADRAY_BUILD_SHADER_COMPILER` | ON |
| `RADRAY_ENABLE_SHADER_JIT`、`RADRAY_BUILD_SHADER_TOOLS` | 默认 ON，要求 shader compiler |
| `RADRAY_ENABLE_PROFILER` | 所有配置默认 ON；链接 Tracy client，见[性能采样](#性能采样tracy) |

验证不带编译器的 runtime 时使用独立目录：

```powershell
cmake --preset win-x64-debug-clangcl -B build_runtime_only -DRADRAY_BUILD_SHADER_COMPILER=OFF
cmake --build build_runtime_only --config Debug --parallel 24
ctest --test-dir build_runtime_only -C Debug --output-on-failure
```

该配置使 JIT/tools 一起关闭，不发现或部署 DXC。检查实际 target 边使用链接 map（MSVC
`/MAP`）或 Ninja 的 `ninja -C build_runtime_only -t commands`；Vulkan 由 volk 加载，
`dumpbin /DEPENDENTS` 不能证明静态库依赖隔离。

## 性能采样（Tracy）

`RADRAY_ENABLE_PROFILER` 默认 ON，保留 Tracy client、CPU frame/phase zones 和后端 GPU 时间戳。
viewer 版本应与 project_manifest.json 的 Tracy tag 匹配；client 只监听 localhost、按需连接。
业务插桩只用 radray/profiler.h 宏，详见 [Core](../architecture/core-facilities.md#性能采样宏)。
CPU record/Submit 时间与 GPU 时间线分开解读。关闭使用 `-DRADRAY_ENABLE_PROFILER=OFF`。
旧 RenderGraph/Forward 的 profile/record harness、逐命令采样开关及 RG plots 已移除。

## 测试

先完成构建再运行 CTest，不并发执行两者。`-R` 匹配注册用例名中的 gtest suite，
不是 CMake target；以下是常用目标与 suite 的对应关系，可能还包含同一目标内的其他 suite：

`cmake --build build_release --config Release --target radray_runtime_tests --parallel 4`
构建当前配置启用的全部 runtime 测试可执行文件，共享依赖只调度一次。完成后用
`ctest --test-dir build_release/modules/runtime/tests -C Release --output-on-failure` 执行该目录的完整回归。

| CMake target | `ctest -R` 示例 |
|---|---|
| `test_runtime_type` | `RuntimeTypeIdTest` |
| `test_asset_slot` | `AssetSlotTest` |
| `test_frame_upload` | `FrameUploadTest` |
| `test_flight_completion` | `FlightCompletionTest` |
| `test_asset_database` | `AssetDatabaseTest` |
| `test_component_rtti` | `ComponentRttiTest` |
| `test_render_pass_registry` | `RenderPassCacheKeyTest`, `FramebufferCacheKeyTest`, `RenderPassRegistryTest` |
| `test_device_capabilities` | `TextureDescriptorValidation`, `DeviceCapabilitiesTest` |
| `test_gpu_test_fixture` | `GpuTestFixture`, `GpuValidationProbe` |
| `test_spot_light` | `SpotLight` |
| `test_shader_parameters` | `RadRayRuntimeShaderParameters` |

D3D12 descriptor table 回归由 `test_radray_render_d3d12_layout` 的 `D3D12DeviceFixture`、
`DescriptorDirtyRangesD3D12Test` 覆盖；启用 shader compiler 时，`test_radray_render_pso_smoke` 的
`RadRayRenderPsoSmoke.D3D12VisibilityTablesAndExplicitMirrorsDraw` 和
`RadRayRenderPsoSmoke.D3D12DirtyArraysTextureAndSamplerDispatch` 检查真实 draw/dispatch 读回。
`RadRayRenderPsoSmoke.VulkanImmediateDescriptorsAndDynamicOffsets` 检查 Vulkan Set 即时更新、数组部分更新、
typed buffer view 替换与创建失败恢复，以及乱序 dynamic offsets；省略 Flush 的轮次也必须得到新值。
GPU 验证可设置 `RADRAY_TEST_GPU_VALIDATION=1`，并以 `RADRAY_TEST_REQUIRED_BACKENDS=d3d12`
避免缺少设备或验证层时静默跳过。

descriptor 发布微基准默认禁用，不计入普通测试。先构建 Release 的
`test_radray_render_d3d12_layout`，然后显式运行：

```powershell
build_release/_build/Release/test_radray_render_d3d12_layout.exe --gtest_filter=*DescriptorPublishBenchmark --gtest_also_run_disabled_tests --gtest_repeat=5
```

输出 scenario 0–5 分别表示单 CBV、连续 64 元素、64 元素中每隔 7 项更新、重复相同 Set、
Vertex/Pixel 各 32 元素、显式 Vertex/Pixel 两个目标各 64 元素。
`total_ns` 是每轮 Set 与 Flush 总耗时，`flush_ns` 单独计时 Flush；`allocation_ns` 是创建并销毁
一个 set 的平均耗时。比较时固定配置、设备和验证层设置；此基准启用 D3D12 debug layer，GPU-based
validation 由上述环境变量控制。基准不提交 GPU 命令，不代表 draw/dispatch 或 GPU 执行时间。
| `test_runtime_shader_jit` | `RadRayRuntimeShaderJit` |
| `test_application` | `RuntimeFoundation`（双后端、单/双线程窗口与原生录制） |
| `test_radray_render_shader_artifact` | `RadRayRenderShaderArtifact` |
| `test_radray_shader_contract` | `RadRayShaderContract` |
| `test_radray_render_shader_layout` | `RadRayRenderShaderLayout` |
| `test_radray_render_d3d12_layout` | `D3D12DeviceFixture` |
| `test_radray_render_vulkan_layout` | `VulkanDeviceFixture` |
| `test_radray_render_pso_smoke` | `RadRayRenderPsoSmoke` |
| `test_radray_shader_compiler_client` | `RadRayShaderCompilerClient` |
| `test_radray_dxc_metadata` | `RadRayDxcMetadata` |
| `test_shaderlib_passes` | `RadRayShaderLibPass` |

```powershell
cmake --build build_debug --config Debug --target test_asset_slot --parallel 24
ctest --test-dir build_debug -C Debug -R AssetSlotTest --output-on-failure
```

全量构建后去掉 `-R` 可运行全部已注册测试。GPU 用例区分后端未编译、无 adapter、缺验证层与
初始化失败；可选后端不可用时可 `GTEST_SKIP()`，native 初始化、资源、PSO、提交或读回错误必须
失败。`RADRAY_TEST_REQUIRED_BACKENDS=d3d12,vulkan` 使必测后端缺失也失败。样例资产在被忽略的 `assets/`
下，通过源码仓库外的渠道准备。新增测试源文件后重新 configure；CTest 不负责构建。

涉及 RTTI、公共 C++ ABI 或跨静态库对象查询时，Debug 与 Release 都要分别完成全量构建，
再运行各自配置的测试。其他改动选择相关 suite 验证，不复用旧会话的通过计数。

`radray_add_test` 在链接后做 POST_BUILD discovery（`radray_gtest_discover_tests`），把每个
`TEST()` 写成独立 CTest 用例；JSON 输出目录按 target 隔离，避免 CMake 4.4 同目录并行
POST_BUILD 争用 `cmake_test_discovery_<hash>.json`。不要改回 `DISCOVERY_MODE PRE_TEST`：
CMake 4.4 的 PRE_TEST 每次启动 CTest 都对全部测试 exe 跑 `--gtest_list_tests` 且不缓存，
VSCode CMake 插件点一项也会先付这整笔发现时间。新增或改名 `TEST()` 后要重新链接对应
target，CTest 不负责发现。修改注册逻辑时，比较各 exe 的 `--gtest_list_tests` 与 CTest
列表及实际命令，不能仅凭 `ctest -N` 的总数判断正确性。注册写法见 [C++ 约定](cpp-conventions.md)。

`tools/run_render_validation.py` 对已构建配置串行运行 CTest，保存每用例 gtest XML、CTest JUnit、日志
和汇总 JSON。每份结果记录 SHA、未提交改动摘要、配置开关、OS/驱动以及 fixture 提供的 backend、
adapter 和 validation 属性；required backend 必须实际执行，通过总数不能掩盖后端全跳过。输出目录
必须为空，失败与环境失败单独统计。返回非零时先检查 `summary.json` 和对应 XML/日志。

```powershell
python tools/run_render_validation.py --self-test
python tools/run_render_validation.py --build-dir build_debug --config Debug --output-dir validation/debug --required-backends d3d12,vulkan
cmake --build build_debug --config Release --parallel 24
python tools/run_render_validation.py --build-dir build_debug --config Release --output-dir validation/release --required-backends d3d12,vulkan
python tools/run_render_validation.py --build-dir build_debug --config Debug --output-dir validation/gpu-check --regex "RadRayRenderPsoSmoke|RadRayRuntimeShaderJit" --gpu-validation
```

正常 GPU fixture 开启 D3D debug layer 或 Vulkan validation + synchronization validation。
`--gpu-validation` 另启 D3D GBV / Vulkan GPU-assisted validation，只用于少量数值用例；延迟 host-signaled
fence 压力和性能基准独立运行，避免验证层 semaphore 跟踪阻塞影响压力协议。H04 在独立测试进程注入
一条原生回调错误，单独记为 expected probe；普通验收的 unexpected validation errors 必须为零。
输入校验中的预期拒绝不等同于 native validation 错误。

无 JIT 配置仍运行资产、flight、组件与 shader 参数测试；依赖 compiler 的 GPU shader suite 不注册。
runtime-only 可消费匹配 backend 的已编译 artifact，源码请求不会反向链接 compiler client。

## 渲染框架重构状态

旧 RenderGraph、Forward、ImGui 及其样例和专用测试已移除，相应 CMake 选项不再提供。
基线设计、历史能力与依赖边界见[临时设计快照](../temp/render-framework-design.md)。
通用 CTest 验证脚本、shader CLI、依赖恢复和编译数据库工具保留。

## 编译数据库与文档检查

Ninja 配置已开启 `CMAKE_EXPORT_COMPILE_COMMANDS`，直接让 clangd 指向构建目录，或复制到
IDE 默认读取的 `.vscode/compile_commands.json`：

```powershell
New-Item -ItemType Directory -Force .vscode | Out-Null
Copy-Item -LiteralPath build_debug/compile_commands.json -Destination .vscode/compile_commands.json
```

Visual Studio/MSBuild 构建使用仓库脚本求值项目，不编译源码：

```powershell
python tools/win_gen_compile_commands.py --build-dir build_clangcl --configuration Debug
```

默认输出 `.vscode/compile_commands.json`。IDE 设置见 [开发环境](dev-env.md)。
文档或技能修改运行 `python tools/check_docs.py` 与 `git diff --check`，详见[文档维护](documentation.md)。
