> - 适用: 恢复依赖、配置与构建 RadRay、运行测试或生成编译数据库
> - 权威: 本文说明仓库构建操作；选项与预设以锚点中的 CMake 文件为准
> - 锚点: `CMakeLists.txt`, `CMakePresets.json`, `cmake/Utility.cmake`, `cmake/libjpeg.cmake`, `project_manifest.json`, `tools/fetch_third_party.py`, `tools/fetch_sdks.py`, `tools/win_gen_compile_commands.py`, `modules/*/tests/CMakeLists.txt`

# 构建与测试

命令在仓库根执行。Windows Ninja 预设需要已配置 x64 C++ 工具链的终端，以及 CMake、Ninja、
Python 和 Git；开启测试时要求 CMake >= 4.4.1。现有构建目录的 generator 以其 `CMakeCache.txt` 为准，不直接用另一 generator
覆盖；需要切换时用 `-B` 指定独立构建目录。

## 恢复与构建

```powershell
python tools/fetch_third_party.py restore
python tools/fetch_sdks.py restore
cmake --preset win-x64-debug
cmake --build build_debug --config Debug --parallel 24
```

恢复脚本读取 `project_manifest.json`。第三方源码进入 `third_party/`，RadRay DXC package
进入 `SDKs/radray_dxc/extracted`；不手改这些目录。IJG libjpeg 由 `cmake/libjpeg.cmake`
在本工程内编译。shader compiler 开启时需要匹配的 fork
package，stock DXC 不能替代。包的 ABI 与部署边界见 [Shader pipeline](../architecture/shader-pipeline.md)。

`fetch_third_party.py restore` 优先恢复已有 `project_manifest.lock.json` 中的提交；没有 lockfile
时按 manifest 恢复，已安装的默认分支或显式 `Branch` 不会自动检查远端更新。需要更新这些可变引用时运行：

```powershell
python tools/fetch_third_party.py restore --apply
```

`restore --apply` 与 `restore --update` 都会拉取远端默认分支或指定 `Branch` 的最新提交，固定
`Tag` / `Commit` 保持不变。已有 lockfile 的对应条目在安装成功后同步更新；没有 lockfile 时不会
自动生成。`--only xxHash` 可限定依赖，`--no-lock` 可跳过 lockfile；默认分支更新支持 detached HEAD，
并重新读取远端 HEAD，远端切换默认分支后也能跟随。升级固定版本使用 `upgrade --apply`。

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
之前全局定义下列宏，以换取可用的 Debug 帧率：

| 宏 | 作用 | 保留的检查 |
|---|---|---|
| `_ITERATOR_DEBUG_LEVEL=1`（仅 MSVC） | 关闭完整的 iterator debugging，保留 checked iterators | checked iterator 的边界检查 |

`_ITERATOR_DEBUG_LEVEL` 影响 STL 对象布局，必须在同一二进制的所有 C++ TU 一致，所以在此全局设置，
第三方库随工程编译时自动继承；预编译的 DXC package 不经本工程编译，不受影响。
不在单个 target 上改写它。`RelWithDebInfo`/`MinSizeRel` 不受此配置影响。

### 预编译头与 Unity Build

不设置全局 `CMAKE_UNITY_BUILD`。core 与 `test_inline_vector` 都不参与。

`radrayrender` 和 `test_radray_render` 只预编译 `<radray/render/rhi.h>`。D3D12 与 Vulkan 头不放进这份预编译头，因为一个目标只能有一份。

`radrayruntime` 和 `test_radray_runtime` 用 `radray_enable_unity_build` 把 `.cpp` 按每批 32 个合成编译，MSVC 加 `/bigobj`。`radrayruntime` 的预编译头是 PUBLIC。链上它的测试、example 和 benchmark 会继承这份头列表，并各自再编一份。列表收录 runtime 会包含的 core 公开头，以及 core 以 PUBLIC 依赖暴露的第三方头：Eigen、fmt、magic_enum、sigslot；并保留 `<radray/render/rhi.h>`。不包含 `<radray/coroutine.h>`：stdexec 用泛型 lambda 作模板参数，clang-cl 的预编译头会把该 lambda 和翻译单元里的当成不同类型。测试目标再加 `<gtest/gtest.h>`。`radray/platform/win32_headers.h` 不进入这份预编译头。改其中一个 `.cpp` 会重编它所在的整批。同名 `static` 或匿名命名空间符号冲突时，给该文件设置 `SKIP_UNITY_BUILD_INCLUSION`。同一批里前面的文件引入的宏会作用到后面的文件。Windows SDK 把 `small` 定义成 `char`；`win32_headers.h` 会取消它，但之后再包含 COM 或 D3D 头可能再次定义，所以不要用 `small` 当标识符。

## 常用配置边界

| 开关 | 默认与依赖 |
|---|---|
| `RADRAY_BUILD_TESTS` | ON |
| `RADRAY_BUILD_EXAMPLES` | ON；要求 runtime 与 shader JIT，关闭任一依赖时须显式 OFF |
| `RADRAY_BUILD_BENCHMARKS` | 单配置 Release 或包含 Release 的多配置 generator 默认 ON |
| `RADRAY_BUILD_WINDOW`、`RADRAY_BUILD_RENDER` | ON |
| `RADRAY_BUILD_RUNTIME` | 默认 ON，要求 render 和 window |
| `RADRAY_ENABLE_D3D12` | Windows 且 render 开启时默认 ON |
| `RADRAY_ENABLE_VULKAN` | render 开启时默认 ON |
| `RADRAY_BUILD_SHADER_COMPILER` | ON |
| `RADRAY_ENABLE_SHADER_JIT`、`RADRAY_BUILD_SHADER_TOOLS` | 默认 ON，要求 shader compiler |
| `RADRAY_ENABLE_PROFILER` | 所有配置默认 ON；链接 Tracy client，见[性能采样](performance.md#性能采样tracy) |

验证不带编译器的 runtime 时使用独立目录：

```powershell
cmake --preset win-x64-debug-clangcl -B build_runtime_only -DRADRAY_BUILD_SHADER_COMPILER=OFF -DRADRAY_BUILD_EXAMPLES=OFF
cmake --build build_runtime_only --config Debug --parallel 24
ctest --test-dir build_runtime_only -C Debug --output-on-failure
```

该配置使 JIT/tools 一起关闭，不发现或部署 DXC。检查实际 target 边使用链接 map（MSVC
`/MAP`）或 Ninja 的 `ninja -C build_runtime_only -t commands`；Vulkan 由 volk 加载，
`dumpbin /DEPENDENTS` 不能证明静态库依赖隔离。

## 性能测量

Tracy、CPU/GPU 基准、配对测量与内存口径统一见[性能测量](performance.md)。
先完成构建与正确性回归，再串行运行 benchmark；历史报告由[架构地图](../architecture/overview.md#历史资料)索引。

## 测试

先完成构建再运行 CTest，不并发执行两者。`-R` 匹配注册用例名中的 gtest suite，
不是 CMake target；以下是常用目标与 suite 的对应关系，可能还包含同一目标内的其他 suite：

`cmake --build build_release --config Release --target test_radray_runtime --parallel 4`
构建 runtime 测试可执行文件。同一模块的 GTest 源文件链进一个 exe：`test_radray_core`、
`test_radray_render`、`test_radray_runtime`、`test_radray_window`（Win32）、`test_radray_shader_compiler`。`test_inline_vector`
替换全局 `operator new`，单独成 exe。`radray_runtime_tests` 只依赖 `test_radray_runtime`。
完成后用 `ctest --test-dir build_release/modules/runtime/tests -C Release --output-on-failure`
执行该目录的完整回归。CTest 仍是每个 `TEST()` 一个进程；直接运行合并后的 exe 会在同一进程跑该模块全部用例。

| 源文件 | `ctest -R` 示例 |
|---|---|
| `test_runtime_type.cpp` | `RuntimeTypeIdTest` |
| `test_manual_coroutine_scheduler.cpp` | `ManualCoroutineScheduler`（冻结派发、取消其他等待者、跨等待表截止与延迟取消） |
| `test_asset_slot.cpp` | `AssetSlotTest` |
| `test_frame_upload.cpp` | `FrameUploadTest` |
| `test_runtime_services.cpp` | `RuntimeServices`（关闭 Scene/World，双后端单/双线程 shader 与原生命令读回） |
| `test_event_pump.cpp`（window） | `NativeEventPumpTest`（延迟 FIFO、文本寿命、冻结派发、注销身份、有限采集、唤醒与即时兼容） |
| `test_application.cpp`, `test_application_loop.cpp` | `RuntimeFoundation`（双后端单/双线程、F=1/2/3/4、GT/RT 重叠、同步窗口消息、真实 GPU gate、独立队列排空、模态/停止/输入） |
| `test_application_systems.cpp` | `ApplicationSystems`（零系统、单系统、CPU 帧循环的完成、等待与延迟、非法 `FlightDataCount` 与 Render/Gpu 配置） |
| `test_gpu_system.cpp` | `GpuSystemTest`, `GpuSystemDeathTest` |
| `test_scene_delivery.cpp` | `SceneDelivery`, `SceneDeliveryRunner`, `MultiWorldSceneRunner`（CPU 场景帧循环与双后端 GPU runner，F=1/2/3/8） |
| `test_scene_delivery_state.cpp` | `SceneDeliveryState`（直接通过 SceneManager 验证发布次序、serial、停止排空和随机槽位复用） |
| `test_world_scenes.cpp` | `WorldManager`, `WorldScenes`（多 World 所有权、暂停、独立场景写入、连接与退休） |
| `test_scene_updates.cpp` | `SceneUpdates`（组件标脏合并、生命周期、代次与收集约束；纯 CPU） |
| `test_scene_assets.cpp` | `SceneAssets`（类型无关的资产常驻/退休、Ready 通知、共享等待取消与 GT 释放；纯 CPU） |
| `test_static_mesh_scene.cpp` | `StaticMeshScene`（CPU mesh 描述、变换/bounds、替换/删除与持久描述；无 GPU 资源） |
| `test_scene_sync.cpp` | `SceneSyncCorrectness`（World → RenderScene 正确性） |
| `test_frame_scenarios.cpp` | `FrameScenarios`（Tick 驱动的巡游、交火、流式、群体、暂停、过场灯光与编辑器拖拽；F=2） |
| `test_multi_window.cpp` | `RuntimeMultiWindow`（三窗口交换链、有序提交与生命周期） |
| `test_asset_database.cpp` | `AssetDatabaseTest` |
| `test_component_rtti.cpp` | `ComponentRttiTest` |
| `test_render_scope.cpp` | `RenderScopeKeys`, `Backends/RenderScopeTest` |
| `test_device_capabilities.cpp` | `TextureDescriptorValidation`, `DeviceCapabilitiesTest` |
| `test_gpu_test_fixture.cpp` | `GpuTestFixture`, `GpuValidationProbe` |
| `test_spot_light.cpp` | `SpotLight` |

D3D12 descriptor table 回归在 `test_radray_render` 中，由 `D3D12DeviceFixture`、
`DescriptorDirtyRangesD3D12Test` 覆盖；启用 shader compiler 时，同一可执行文件的
`RadRayRenderPsoSmoke.D3D12VisibilityTablesAndExplicitMirrorsDraw` 和
`RadRayRenderPsoSmoke.D3D12DirtyArraysTextureAndSamplerDispatch` 检查真实 draw/dispatch 读回。
`RadRayRenderPsoSmoke.VulkanImmediateDescriptorsAndDynamicOffsets` 检查 Vulkan Set 即时更新、数组部分更新、
typed buffer view 替换与创建失败恢复，以及乱序 dynamic offsets；省略 Flush 的轮次也必须得到新值。
GPU 验证可设置 `RADRAY_TEST_GPU_VALIDATION=1`，并以 `RADRAY_TEST_REQUIRED_BACKENDS=d3d12`
避免缺少设备或验证层时静默跳过。

descriptor 发布与分配微基准见[性能测量](performance.md#descriptor-发布与分配)。

| 源文件 | `ctest -R` 示例 |
|---|---|
| `test_runtime_shader_jit.cpp` | `RadRayRuntimeShaderJit` |
| `test_application.cpp` | `RuntimeFoundation`（双后端、单/双线程窗口与原生录制，旧槽位复用与后一帧 CPU 录制重叠） |
| `test_radray_render_shader_artifact.cpp` | `RadRayRenderShaderArtifact` |
| `test_radray_shader_contract.cpp` | `RadRayShaderContract` |
| `test_radray_render_shader_layout.cpp` | `RadRayRenderShaderLayout` |
| `test_radray_render_d3d12_layout.cpp` | `D3D12DeviceFixture` |
| `test_radray_render_vulkan_layout.cpp` | `VulkanDeviceFixture` |
| `test_radray_render_pso_smoke.cpp` | `RadRayRenderPsoSmoke` |
| `test_radray_shader_compiler_client.cpp` | `RadRayShaderCompilerClient` |
| `test_radray_dxc_metadata.cpp` | `RadRayDxcMetadata` |
| `test_shaderlib_passes.cpp` | `RadRayShaderLibPass` |

```powershell
cmake --build build_debug --config Debug --target test_radray_runtime --parallel 24
ctest --test-dir build_debug -C Debug -R AssetSlotTest --output-on-failure
```

全量构建后去掉 `-R` 可运行全部已注册测试。GPU 用例区分后端未编译、无 adapter、缺验证层与
初始化失败；可选后端不可用时可 `GTEST_SKIP()`，native 初始化、资源、PSO、提交或读回错误必须
失败。`RADRAY_TEST_REQUIRED_BACKENDS=d3d12,vulkan` 使必测后端缺失也失败。样例资产在被忽略的 `assets/`
下，通过源码仓库外的渠道准备。新增测试源文件后重新 configure；CTest 不负责构建。
Application/GpuSystem 测试直接读取启动状态，仅在后端未编译、无 adapter 或缺验证层且后端未被指定为必测时跳过；
不再为测试先创建探测 device。CPU 用例只启用所需系统；需要 GPU 但不检查 GPU 耗时的用例关闭帧计时器。

`RuntimeMultiWindow` 在一个 Application 内运行主窗口与两个副窗口，覆盖 D3D12/Vulkan 的
单线程和双线程模式。Vulkan 四例要求含同步验证修复的 VVL 1.4.364+，见下方版本说明。用例清屏各交换链，交替正序/逆序归还多命令批次，混合显式与自动提交，
并通过逐批 fence、纯同步批次和 GPU buffer 读回检查提交顺序及完成通知。
生命周期用例覆盖副窗口 resize、隐藏创建后的延迟挂接与显示、detach/reattach、release/reattach、
销毁及重新创建，期间验证其他窗口仍可呈现和提交；最后关闭主窗口，检查 flight 完成消息已排空。
flight 槽位与交换链 buffer 数量刻意不完全相同。用例要求 Windows 桌面，开启 debug/同步验证，
CTest 设置串行执行及每例 90 秒超时。

同一目标的 `RuntimeVulkanSwapChain` 覆盖 image 枚举失败、native 创建已退休旧链后失败的恢复，
以及 CPU 等待提交后 Present、反复立即重建和销毁、等待已完成的旧 timeline 值；三交换链
串联 timeline 提交后统一 Present 的用例也检查 VVL 1.4.364+；`RuntimeVulkanSwapChainLimits` 检查无上限
surface 的 buffer 数量。故障注入用例只允许精确匹配的预期错误，其他原生验证错误仍失败。
设置 `RADRAY_TEST_WSI_TRACE=1` 可记录多窗口 phase、serial、flight、窗口、chain、image 和两个
semaphore，结合验证层中的原生对象名称定位偶发同步错误。
长期重复使用 CTest 的 `--repeat`，每次启动独立进程；不要在同一进程中对全部 GPU 用例执行
大量 `--gtest_repeat`，Tracy 的 GPU context id 在进程内累计，超过 256 会触发其断言。

```powershell
cmake -S . -B build_debug
cmake --build build_debug --config Debug --target test_radray_runtime --parallel 4
$env:RADRAY_TEST_REQUIRED_BACKENDS = "d3d12,vulkan"
ctest --test-dir build_debug -C Debug -R RuntimeMultiWindow --output-on-failure
ctest --test-dir build_debug -C Debug -R 'RuntimeMultiWindow|RuntimeVulkanSwapChain' --repeat until-fail:20 --output-on-failure
```

### Vulkan 多交换链同步验证已知问题

这些回归要求包含 [Vulkan-ValidationLayers #13248](https://github.com/KhronosGroup/Vulkan-ValidationLayers/pull/13248)
修复的校验层。SDK 1.4.357.0 在三交换链、同队列 timeline 串联提交并在 Present 前执行 CPU wait 时，
会出现 [#13117](https://github.com/KhronosGroup/Vulkan-ValidationLayers/issues/13117) 中的
`SYNC-HAZARD-WRITE-AFTER-PRESENT`；测试以 layer specVersion >= 1.4.364 为版本门槛。
runtime 使用正常提交路径，不加空批次 workaround，也不关闭同步验证或过滤该错误。
2026-10-13 对独立修复层的构建与验收结果见[历史实施报告](../history/2026-10-13-gpu-lifecycle-implementation.md)。

以下五例在 `modules/runtime/tests/test_multi_window.cpp` 中正常编译并进入 GTest/CTest discovery；
运行前记录 validation layer specVersion，旧于 1.4.364 的已知问题层明确 skip。缺失校验层或初始化
失败仍按 native fixture 合同报错。此版本门槛只用于这些测试，renderer 的执行 API 仍是 Vulkan 1.2：

- `RuntimeMultiWindow.VulkanSingleThreadOrderedSubmissions`
- `RuntimeMultiWindow.VulkanThreadedOrderedSubmissions`
- `RuntimeMultiWindow.VulkanSingleThreadSwapChainLifecycle`
- `RuntimeMultiWindow.VulkanThreadedSwapChainLifecycle`
- `RuntimeVulkanSwapChain.MultiSwapChainHostWaitBeforePresent`

D3D12 多窗口四例与其余 Vulkan 单交换链、故障恢复和 image count 用例继续执行。
独立原生复现程序及其 `--empty-predecessor` 对照选项保留；该选项只用于
诊断，不是运行时兼容逻辑，也不加入常规回归。

更换校验层时，先用下方原生程序在对应版本上验证默认路径，
再运行已恢复的五例。使用独立 layer 目录时，在调用原生程序和 CTest 的进程环境中设置
`VK_LAYER_PATH` 指向只含有效 layer manifest 和对应 DLL 的目录。
设置 `RADRAY_TEST_REQUIRED_BACKENDS=d3d12,vulkan`，在 Debug/Release 下分别执行上述
`RuntimeMultiWindow|RuntimeVulkanSwapChain` 重复回归；不能以关闭验证或过滤该错误作为恢复条件。

原生 Vulkan 对照程序位于
[`modules/render/tests/vulkan_present_repro/`](../../modules/render/tests/vulkan_present_repro/)。
它是可单独分发的 C 程序，只链接系统 Vulkan loader 与 Win32，不依赖 RadRay、GLFW、GTest、
shader 或第三方容器。为保留仅 Vulkan API 的复现环境，使用该目录的独立 CMake 工程，
不加入仓库的常规 GTest discovery；默认路径在 SDK 1.4.357.0 中复现错误，在已修复层中应返回 0。

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

| 参数 | 差异 | SDK 1.4.357.0 / RTX 4080 历史复现结果 |
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

原生复现排除了 RadRay 封装作为必要触发条件。2026-10-13 复核使用修复合并提交 `da7669a`，
校验源没有修改；当时用 ClangCL 构建时，只从 gpu_av_spirv 静态目标
移除重复编译的 spirv_grammar_helper.cpp，该生成文件仍由 vvl 共享目标编译一次。构建调整、
DLL 哈希、原生日志与实际加载路径当时保存在 `build_release/gpu-lifecycle-vvl-*`，未入库。

### 测试发现与结果记录

涉及 RTTI、公共 C++ ABI 或跨静态库对象查询时，Debug 与 Release 都要分别完成全量构建，
再运行各自配置的测试。其他改动选择相关 suite 验证，不复用旧会话的通过计数。

`radray_add_test` 调用 CMake 的 `gtest_discover_tests`（默认 `POST_BUILD`），在链接后把每个
`TEST()` 写成独立 CTest 用例。`test_radray_runtime` 对需要 `RUN_SERIAL` 和 90 秒超时的 suite 再发现一次，
用 `TEST_FILTER` 与其余用例分开。这要求 CMake 4.4.1 及以上：4.2 至 4.4.0 的 POST_BUILD 不传
`TEST_TARGET`，同目录并行发现会争用 `cmake_test_discovery_e3b0c44298.json`。不要传
`DISCOVERY_MODE PRE_TEST`：CMake 4.4 的 PRE_TEST 每次启动 CTest 都对全部测试 exe 跑
`--gtest_list_tests` 且不缓存，VS Code CMake 插件点一项也会先付这整笔发现时间。新增或改名
`TEST()` 后要重新链接对应 target，CTest 不负责发现。修改注册逻辑时，比较各 exe 的
`--gtest_list_tests` 与 CTest 列表及实际命令，不能仅凭 `ctest -N` 的总数判断正确性。注册写法见
[C++ 约定](cpp-conventions.md)。

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
基线设计、历史能力与依赖边界见[历史设计快照](../history/2026-09-14-render-framework-design.md)。
通用 CTest 验证脚本、shader CLI、依赖恢复和编译数据库工具保留。

## 生命周期与增量渲染验收

Scene GPU 闭环新增 `SceneApplyChanges`、`SceneViews`/`SceneViewsDeathTest`、`Backends/SceneGpu`、
`SceneGpuAllocation`、`SceneGpuLifetime` 与 `SceneDraw`（关闭 JIT 时明确报告跳过）。前者覆盖最终变化集合、视图捕获与修改禁令；GPU 测试覆盖
双后端矩阵读回、F=1/2/3、单/双线程录制、槽位 generation、多 Scene、零上传静止帧、分配失败、扩容和稀疏范围。
SceneDraw 读取真实非对称图像和正面颜色，覆盖对象/相机移动、负缩放绕序、1/3 view、相机销毁、Scene 重连、late-drop 重试与窗口最小化/恢复/resize。
SceneGpuLifetime 用延迟主队列 fence 验证 Scene 删除的退休覆盖；host-signaled fence 压力不与 native validation 混跑。
缺少后端按既有 startup 规则跳过；正式验收设置 `RADRAY_TEST_REQUIRED_BACKENDS=d3d12,vulkan`，JIT 初始化后的失败不跳过。

窗口示例默认 1,000 实例、F=2、单线程、单视图，shader 源码位于 `examples/scene_sync`：

```powershell
cmake -S . -B build_debug -DRADRAY_BUILD_EXAMPLES=ON
cmake --build build_debug --config Debug --target example_scene_sync test_radray_runtime --parallel 12
ctest --test-dir build_debug -C Debug --output-on-failure
build_debug/_build/Debug/example_scene_sync.exe --vulkan --flights=3 --views=3 --instances=1000
```

`--d3d12`/`--vulkan` 选择后端，`--multithread` 改为双线程，`--flights=1/2/3`、`--views=N` 调整配置；
`--valid-layer` 开启验证，`--frames=N` 限定运行帧数。示例沿用原生窗口的 resize、最小化与恢复入口。
窗口标题每 0.5 秒更新一次 FPS，按该时间段内成功 acquire 并完成绘制录制的帧数除以墙钟时间计算，
单线程与双线程均适用；跳过绘制的帧不计数。相机通过 `CameraControl` 控制：左键拖动环绕原点，
右键或中键拖动平移，滚轮向上拉近、向下拉远；失焦或丢失鼠标捕获时清除输入状态。

采样区间、对象上传基准与数据解释见[Scene GPU 上传](performance.md#scene-gpu-上传)。

`WorldLifecycle` 覆盖立即创建、统一 epoch、延迟销毁、连接重入、层级和 typed Light；
`SceneDelivery` / `SceneDeliveryRunner` 覆盖 packet/serial、CPU reader lease 和 F=1/2/3/8。
`FrameScenarios` 用玩家、抛射物、流式块、群体挂件、暂停和编辑器拖拽走 Tick、S1 与 F=2 交付，核对渲染场景里的网格、矩阵、bounds 与光源表。
`GpuSceneLifetime`（启用 JIT）执行 D3D12/Vulkan 三角形 draw/readback、慢 fence、手工 owner、取消与失败的上传；
其中 host-signaled fence 压力与 native validation 数值测试分开运行。

```powershell
cmake --build build_debug --config Debug --target radray_runtime_tests --parallel 12
ctest --test-dir build_debug -C Debug -R "WorldLifecycle|SceneDelivery|FrameScenarios|GpuSceneLifetime|LifecycleScale" --output-on-failure
```

`test_lifecycle_scale.cpp` 中的 `LifecycleScale` 只运行 10k/100k 共享 mesh 与变换增量断言。
生命周期与 OBJ 基准见[性能测量](performance.md#cpu-生命周期与-obj)。

Sanitizer 使用独立 RelWithDebInfo 构建并关闭 mimalloc/profiler；MSVC ASan 添加 `/fsanitize=address`，
不与 `/RTC1` 混用。ClangCL 可以同时启用 ASan/UBSan，compiler probe 同样指定 RelWithDebInfo 与非 Debug CRT。
历史生命周期验收范围见[生命周期 v2 验收](../history/2026-09-20-lifecycle-v2-validation.md)，其中旧 CSV 不代表当前基准协议。

## Pipeline 与命令存储验证

```powershell
cmake --build build_debug --config Debug --parallel 12
$env:RADRAY_TEST_REQUIRED_BACKENDS = 'd3d12,vulkan'
ctest --test-dir build_debug -C Debug -R 'CommandAllocator|GpuSystem|Pipeline|SceneDraw|FrameUploadTest|SceneViews' --output-on-failure
```

命令存储和实际 pipeline 的性能边界见[性能测量](performance.md#命令存储与绑定)。

## RHI 校验层验收

开关、失败语义和生命周期边界见 [RHI 校验契约](../architecture/render-rhi.md#可选-rhi-校验)。
`GpuOptions::RhiValidation` / `GpuSystemDescriptor::RhiValidation` 与原生验证开关独立。
`RhiValidationTest` 使用 fixture/probe 检查完整解包、全批次预检、拒绝后重试、handle、encoder 所有权、
frame token 与并发注册表；`RhiValidationDeathTest` 覆盖非法析构、没有失败通道的查询、非法 encoder 终结与 frame 消费。
EndRenderPass/EndComputePass 及 Present/DiscardAcquiredFrame 按值转移所有权，校验失败立即终止；不属于可拒绝后重试的操作。
`BackendsAndModes/RhiValidationGpuTest` 覆盖两个后端的校验开/关路径；PSO smoke 同样运行两种模式。
原生对象检查和手工 signal gate 的测试显式关闭 RHI 包装，普通误用由校验层测试负责。

```powershell
cmake --build build_debug --config Debug --parallel 12
$env:RADRAY_TEST_REQUIRED_BACKENDS = 'd3d12,vulkan'
ctest --test-dir build_debug -C Debug -R 'RhiValidation|CommandAllocator|RenderScope|RadRayRenderPsoSmoke' --output-on-failure
ctest --test-dir build_debug -C Debug --output-on-failure
cmake --build build_debug --config Release --target test_radray_render test_radray_runtime bench_command_allocator bench_d3d12_descriptor --parallel 12
ctest --test-dir build_debug -C Release -R 'RhiValidation|CommandAllocator|RenderScope|RadRayRenderPsoSmoke' --output-on-failure
```

单后端和无 compiler 组合用独立目录，保持同一 generator，先构建再测试；示例为 D3D12：

```powershell
cmake -S . -B build_rhi_d3d12 -G "Visual Studio 18 2026" -T ClangCL `
  -DRADRAY_ENABLE_VULKAN=OFF -DRADRAY_ENABLE_D3D12=ON -DRADRAY_BUILD_SHADER_COMPILER=OFF `
  -DRADRAY_BUILD_EXAMPLES=OFF -DRADRAY_BUILD_BENCHMARKS=OFF
cmake --build build_rhi_d3d12 --config Debug --target test_radray_render test_radray_runtime --parallel 12
$env:RADRAY_TEST_REQUIRED_BACKENDS = 'd3d12'
ctest --test-dir build_rhi_d3d12 -C Debug -R 'RhiValidation|CommandAllocator|RenderScope' --output-on-failure
```

Vulkan 对照交换两个后端选项，使用 `build_rhi_vulkan` 和 required backend `vulkan`。
完整验收恢复 required backend 为当前构建的集合，未编译后端按既有 startup 规则报告跳过。

RHI 校验开关的开销、绑定吞吐与对象大小测量见[性能测量](performance.md#命令存储与绑定)。

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

## GPU 生命周期回归

必要更新、owner 与完成账本的修改使用实际 GPU 回归：

```powershell
cmake --build build_debug --config Debug --parallel 12
ctest --test-dir build_debug -C Debug --output-on-failure -R 'RenderScope|FrameTimeline|FrameResources|GpuSystem|GpuRetainedResult|SceneGpu|SceneDraw|RasterDraw|Application'
```

FrameTimeline 与 SceneGpu 覆盖 F=1/2/3/4；GpuRetainedResult 使用原生 gate 验证 CPU 取消和延迟消费，测试不用等待时长猜测完成。完整回归仍执行无 -R 的 ctest。旧 owner factories、registry、Scene Attempt、资产 GPU deferred 与 uploader flight 路由已移除。

配对对照、完整压力覆盖与 Tracy 回归见[性能测量](performance.md#gpu-生命周期对照)。
