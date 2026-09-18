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
| `test_gpu_system` | `GpuSystemTest`, `GpuSystemDeathTest` |
| `test_scene_delivery` | `SceneDelivery`（含 CPU 交付契约与 D3D12/Vulkan runner，F=1/2/3） |
| `test_scene_updates` | `SceneUpdates`（组件标脏合并、生命周期、代次与收集约束；纯 CPU） |
| `test_scene_assets` | `SceneAssets`（flight 资产保活、Ready 通知、共享等待取消与 GT 释放；纯 CPU） |
| `test_static_mesh_scene` | `StaticMeshScene`（CPU mesh 描述、变换/bounds、替换/删除与持久描述；无 GPU 资源） |
| `test_multi_window` | `RuntimeMultiWindow`（三窗口交换链、有序提交与生命周期） |
| `test_flight_completion` | `FlightCompletionTest` |
| `test_asset_database` | `AssetDatabaseTest` |
| `test_component_rtti` | `ComponentRttiTest` |
| `test_render_pass_registry` | `RenderPassCacheKeyTest`, `FramebufferCacheKeyTest`, `RenderPassRegistryTest` |
| `test_device_capabilities` | `TextureDescriptorValidation`, `DeviceCapabilitiesTest` |
| `test_gpu_test_fixture` | `GpuTestFixture`, `GpuValidationProbe` |
| `test_spot_light` | `SpotLight` |

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

`RuntimeMultiWindow` 在一个 Application 内运行主窗口与两个副窗口，覆盖 D3D12/Vulkan 的
单线程和双线程模式（Vulkan 四例目前暂时注释，见下方已知问题）。用例清屏各交换链，交替正序/逆序归还多命令批次，混合显式与自动提交，
并通过逐批 fence、纯同步批次和 GPU buffer 读回检查提交顺序及完成通知。
生命周期用例覆盖副窗口 resize、隐藏创建后的延迟挂接与显示、detach/reattach、release/reattach、
销毁及重新创建，期间验证其他窗口仍可呈现和提交；最后关闭主窗口，检查 flight 完成消息已排空。
flight 槽位与交换链 buffer 数量刻意不完全相同。用例要求 Windows 桌面，开启 debug/同步验证，
CTest 设置串行执行及每例 90 秒超时。

同一目标的 `RuntimeVulkanSwapChain` 覆盖 image 枚举失败、native 创建已退休旧链后失败的恢复，
以及 CPU 等待提交后 Present、反复立即重建和销毁、等待已完成的旧 timeline 值；三交换链
串联 timeline 提交后统一 Present 的用例目前暂时注释；`RuntimeVulkanSwapChainLimits` 检查无上限
surface 的 buffer 数量。故障注入用例只允许精确匹配的预期错误，其他原生验证错误仍失败。
设置 `RADRAY_TEST_WSI_TRACE=1` 可记录多窗口 phase、serial、flight、窗口、chain、image 和两个
semaphore，结合验证层中的原生对象名称定位偶发同步错误。
长期重复使用 CTest 的 `--repeat`，每次启动独立进程；不要在同一进程中对全部 GPU 用例执行
大量 `--gtest_repeat`，Tracy 的 GPU context id 在进程内累计，超过 256 会触发其断言。

```powershell
cmake -S . -B build_debug
cmake --build build_debug --config Debug --target test_multi_window --parallel 4
$env:RADRAY_TEST_REQUIRED_BACKENDS = "d3d12,vulkan"
ctest --test-dir build_debug -C Debug -R RuntimeMultiWindow --output-on-failure
ctest --test-dir build_debug -C Debug -R 'RuntimeMultiWindow|RuntimeVulkanSwapChain' --repeat until-fail:20 --output-on-failure
```

### Vulkan 多交换链同步验证已知问题

截至 2026-09-17，上游 [Vulkan-ValidationLayers #13117](https://github.com/KhronosGroup/Vulkan-ValidationLayers/issues/13117)
仍为 Open，问题按疑似同步验证误报跟踪，尚不能将根因视为上游已确认。
issue 使用 SDK 1.4.357.0，在三交换链、同队列 timeline 串联提交并在 Present 前执行 CPU wait
时报告 `SYNC-HAZARD-WRITE-AFTER-PRESENT`；完整环境与观察结果以上游 issue 为准。

已撤销 `798bd477` 引入的运行时空批次 workaround：`QueueVulkan::Submit` 恢复单批次提交，
删除仅为该 workaround 保存的同步验证状态与 `RADRAY_TEST_REPRO_SYNCVAL_PRESENT` 测试开关。
同步验证仍开启，验证错误仍使测试失败。交换链重建失败恢复、无上限 image count 处理、
原生对象命名和 fence reset 错误检查保留。

以下五例在 `modules/runtime/tests/test_multi_window.cpp` 中暂时注释，不编译、不进入 GTest/CTest
发现，因此通过的常规回归不包含 Vulkan 多窗口覆盖：

- `RuntimeMultiWindow.VulkanSingleThreadOrderedSubmissions`
- `RuntimeMultiWindow.VulkanThreadedOrderedSubmissions`
- `RuntimeMultiWindow.VulkanSingleThreadSwapChainLifecycle`
- `RuntimeMultiWindow.VulkanThreadedSwapChainLifecycle`
- `RuntimeVulkanSwapChain.MultiSwapChainHostWaitBeforePresent`

D3D12 多窗口四例与其余 Vulkan 单交换链、故障恢复和 image count 用例继续执行。
`2389c3af` 添加的独立原生复现程序及其 `--empty-predecessor` 对照选项保留；该选项只用于
诊断，不是运行时兼容逻辑，也不加入常规回归。

上游明确修复版本或同步要求后，先用下方原生程序在对应校验层版本上验证默认路径，
再取消上述五例的注释，重新构建 `test_multi_window` 以刷新 POST_BUILD discovery。
设置 `RADRAY_TEST_REQUIRED_BACKENDS=d3d12,vulkan`，在 Debug/Release 下分别执行上述
`RuntimeMultiWindow|RuntimeVulkanSwapChain` 重复回归；不能以关闭验证或过滤该错误作为恢复条件。

原生 Vulkan 对照程序位于
[`modules/render/tests/vulkan_present_repro/`](../../modules/render/tests/vulkan_present_repro/)。
它是可单独分发的 C 程序，只链接系统 Vulkan loader 与 Win32，不依赖 RadRay、GLFW、GTest、
shader 或第三方容器。为保留仅 Vulkan API 的复现环境，使用该目录的独立 CMake 工程，
不加入仓库的常规 GTest discovery；默认路径是预期报错的诊断程序，不作为绿色回归用例。

```powershell
pwsh -File modules/render/tests/vulkan_present_repro/reproduce.ps1 -Sdk C:/VulkanSDK/1.4.357.0
```

脚本先构建 Debug/Release，再各启动独立进程运行四组对照，每组默认 5 次；日志与 `results.csv`
位于 `build_vk_present_repro/`。也可手动构建并仅运行一组：

```powershell
cmake -S modules/render/tests/vulkan_present_repro -B build_vk_present_repro -G "Visual Studio 18 2026" -A x64 -DVulkan_INCLUDE_DIR=C:/VulkanSDK/1.4.357.0/Include -DVulkan_LIBRARY=C:/VulkanSDK/1.4.357.0/Lib/vulkan-1.lib
cmake --build build_vk_present_repro --config Debug
cmake -E env VK_LAYER_PATH=C:/VulkanSDK/1.4.357.0/Bin build_vk_present_repro/Debug/vulkan_present_repro.exe
cmake -E env VK_LAYER_PATH=C:/VulkanSDK/1.4.357.0/Bin build_vk_present_repro/Debug/vulkan_present_repro.exe --empty-predecessor
```

默认循环为：Acquire 三个窗口图像 → 交替正反序提交 → 最后一个纯 timeline 同步批次 →
CPU `vkWaitSemaphores` → 按提交顺序逐个 Present。每个绘制批次等待对应 acquire binary semaphore
及前一个 timeline 值，执行 `Present/Undefined → TransferDst → Clear → Present`，再 signal
该图像专属的 present binary semaphore 与下一个 timeline 值。所有 wait 和 barrier 使用
`ALL_COMMANDS`；CPU 等待后才复用 command buffer 与 acquire semaphore。全程单线程、单队列。

| 参数 | 差异 | SDK 1.4.357.0 / RTX 4080 本机观察 |
|---|---|---|
| 无 | 三窗口 timeline 串联，12 轮 | Debug/Release 各 5/5 报 `WRITE_AFTER_PRESENT` |
| `--empty-predecessor` | 每次 Submit 前附加无命令、无 wait/signal 的空批次 | 各 5/5 无错误 |
| `--no-timeline-chain` | 移除 GPU timeline wait，保留 signal、CPU wait 及 acquire/present 同步 | 各 5/5 无错误 |
| `--one-window` | 只运行一个窗口，其余流程相同 | 各 5/5 无错误 |

`--rounds N` 调整轮数。程序打印实际加载的校验层 DLL、GPU、每条诊断的 round/phase，
分别汇总 LOOP 和 TOTAL；返回 0 表示无 validation error，1 表示有 error，2 表示初始化、
参数或 Vulkan 调用失败。复现时已开启 synchronization validation，未过滤任何校验错误。
清理采用未启用 maintenance1 时常见的 `vkDeviceWaitIdle` 路径，其规范保证的局限见
[Khronos WSI 说明](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html)；本例报错发生在
`phase=present`，先于清理，不以清理无报错作为资源生命周期正确性的证明。

原生复现排除了 RadRay 封装作为必要触发条件，但单凭空批次消除报错不能证明校验层根因。
进一步验证应保持此程序与驱动不变，对比同一 VVL 基线的原版和仅补充
`ResolveSubmitSemaphoreWait` 普通信号量分支中 `last_synchronized_present` 传播的版本，
同时运行 VVL 的正、负同步测试，排除补丁只是不再检测错误。本仓库尚未完成该校验层补丁实验。

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
