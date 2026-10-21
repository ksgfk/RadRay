> - 适用: 2026-10-16 Render Framework 重构的实施与前后实测
> - 权威: 经用户授权保存的验收证据；当前契约见架构和指南，不以历史数值作为通用保证
> - 锚点: `modules/runtime/src/render_framework/renderer.cpp`, `modules/runtime/src/render_framework/binding_registry.cpp`, `modules/runtime/tests/test_renderer_registry.cpp`, `examples/descriptor_reuse/`, `tools/run_descriptor_reuse.py`

# Render Framework 重构验收

状态：已实施并完成本机 descriptor 与生命周期验收；Release 性能配对完成，仍存在 CPU 时间回退。完整回归有 6 项环境或后端适用性跳过，均单列。
基线：`f20c56fbf073154185ae792daac71982e5f6ab43` 加实施开始时的 sampler 工作区改动。
删除旧代码之前已保存[原状、计划和补丁](2026-10-16-render-framework-refactor-plan.md)，并实际完成旧框架离屏读回。

当前契约见 [Render Framework](../architecture/render-framework.md)、[帧与 GPU](../architecture/frame-and-gpu.md)、
[Shader authoring](../guide/shader-authoring.md)、[构建与测试](../guide/build-test.md)和[性能测量](../guide/performance.md)。

## 已实现的边界

- Application 独占 Renderer；独立宿主可直接装配。Renderer 借用 GpuSystem 和 RenderSystem，独占材质版本、资源、完整参数组、Scene GPU 镜像和已接纳帧。
- TemplateId、MaterialId、ResourceId、DescriptorBlockId 验证 Renderer、槽位和 generation。句柄复制不管理寿命；注册依赖和帧使用清单由登记表集中管理。
- Material 只保存不可变 CPU 描述与资产依赖。删除 MaterialGpuState、MaterialPass 共享定义、pass 惰性 native plan、sampler 专用共享缓存、RenderServiceGeneration 和旧逐 pass Prepare 路径。
- SceneUpdateBatch 独占材质列表、实例 schema 和连续参数 bytes，更新引用包内 ID、offset/size；RT 保存自己的表示。CPU-only Scene 不要求 Renderer。
- 各 pipeline 的 Plan 以独占对象进入同一个 FrameBuilder，全部收集完成后统一 Compile。录制只访问编译结果；编译失败仍保留已经接纳的必要上传及输出收尾工作。
- 严格校验 Scene/Frame、View/Pass、Material、Sampler 域。混合域 shader 必须迁移 ABI；实际组号仍来自 artifact。
- BindingRegistry 根据后端兼容的完整组与规范化最终值复用参数集。先复用 CPU bytes，再分配 GPU 切片；静态材质不以 view/pass/flight 身份分配。
- 闲置预算为 4096 resource、128 sampler descriptor，另有 4096 组上限；活跃项固定，压力清理闲置后重试。没有增加 D3D12 heap 容量。
- 真正的 queue 完成与 command epoch 退休后，GpuSystem 释放 CompletionLease；Renderer 在所有者安全点消费消息。外部资产退出必须等 Renderer 确认，内部 Scene 资源退出不会积累外部通知。
- 仓库 examples、benchmarks、测试和 shader 已直接迁移；旧参数池和兼容执行路径已删除。资产系统与共享 Device 边界保留。

## 离屏前后数据

同机 NVIDIA GeForce RTX 3060、驱动 591.44、Windows、MSVC 14.51.36231、Debug。
默认 128 对象、32 材质、3 view、3 flight，256×256；预热 `2×F` 帧后运行 30 帧。
每个 case 为独立进程，双后端各 21 个，串行运行。D3D12 resource/sampler heap 仍为 65536/2048。
Vulkan 报告实际 set 和各 descriptor 类型数量，没有估算驱动内部字节数。

| 数据 | 附件 |
|---|---|
| 原始基线，含源码/二进制 SHA-256、命令和配置 | [baseline.json](2026-10-16-render-framework-refactor-baseline.json)、[baseline.csv](2026-10-16-render-framework-refactor-baseline.csv) |
| 全部迁移后的最终原始数据和 manifest | [final.json](2026-10-16-render-framework-refactor-final.json)、[final.csv](2026-10-16-render-framework-refactor-final.csv) |
| 每个场景的稳态前后配对 | [comparison.csv](2026-10-16-render-framework-refactor-comparison.csv) |

以下为 D3D12 稳态实际 table 槽位，包含空洞；数字为 resource/sampler：

| 场景 | 基线 | 最终 | 基线完整组冗余份数 → 最终 |
|---|---:|---:|---:|
| 默认共享定义 | 100/2 | 72/2 | 6 → 0 |
| 独立构造等价定义 | 1216/2 | 72/2 | 564 → 0 |
| Color + Depth | 136/2 | 72/2 | 15 → 0 |
| 兼容但 PSO 不同的双 Color | 200/4 | 72/2 | 49 → 0 |
| 同值 override | 612/2 | 72/2 | 6 → 0 |
| 真实变化的 override | 116/2 | 74/2 | 6 → 0 |
| 可见列表顺序不同 | 200/4 | 78/2 | 49 → 0 |
| 300 材质，共享定义 | 636/2 | 608/2 | 6 → 0 |
| 300 材质，独立等价定义 | 11400/2 | 608/2 | 5388 → 0 |

`mixed-v1` 单独配对：1728/2 → 72/2，最终 ABI 为 `grouped-v1-migrated-from-mixed-v1`。
它不进入 shader 不变场景的比较；旧 mixed 完整组本来不同，不把内部局部 table 重复计为本次完整组冗余。

F=1/2/3 的默认最终值均为 72/2。V=1/3/6 分别为 68/2、72/2、78/2，增长来自不同 view 数据，静态 Material/Sampler 不变。
M=1 时为 10/1；M=300 时共享与独立定义均为 608/2。真实 override 只增加一个 Material 完整组的 2 个 resource 槽位。
同值 override 不新增存储。可见列表不同的控制场景保留必要的独立切片，GPU 读回验证了区别。

Vulkan 默认最终为 38 个 set：2 个 sampler、32 个 sampled image、35 个 uniform buffer、5 个 storage buffer descriptor。
共享定义、独立等价定义、Color + Depth、兼容双 Color 和同值 override 得到同一组计数。

最终 42 项共读回 1854 帧，检查 262080 个颜色像素和 9216 个深度像素。
颜色包含纹理、nearest/linear sampler、矩阵、实例 tint 和 override 的作用。
全部检查点的兼容完整组冗余为零，语义数据额外切片为零；全部场景稳态 Create/Set/Flush 增量为零，分配失败为零。
这不代表 CPU 编译成本为零，不能推导 FPS。

负载回落测试先以 6 view 运行，再降至 1 view。最终在回落第 1、F、61×F 帧及 drain 后均保留 78/2，处于闲置预算内；
它使用有界缓存，不承诺按旧的 60-flight 时间阈值主动下降到最小值。基线在最后检查点为 76/2。
最终所有场景注销、真实完成并显式清理后 set/resource/sampler 均为零。
预算压力和重试由独立 Registry 测试验证；小样例没有制造 heap 扩容。

完整冻结包位于本机 `build_descriptor_reuse/baseline-validated/` 和 `build_descriptor_reuse/renderer-final-02/`，
含旧/新 exe、DLL、shader、artifact、源码 ZIP、配置、每个进程日志与 JSON。附件 manifest 保存文件校验。
这些大文件未入库；本机包缺失时不能宣称仍能直接重跑旧二进制。
最终样例 exe SHA-256 为 `ce66133a3068916c4d5a94b761237446cbb6863ae6736dbeef74a169e2845081`，
源码 ZIP 为 `4e41eb446c2abdddb24f0d1211577534dfb698319779c6220a15109e3f395dfb`。

```powershell
python tools/run_descriptor_reuse.py --exe build_debug/_build/Debug/example_descriptor_reuse.exe --output build_descriptor_reuse/renderer-final-02
```

输出目录必须全新。重跑时换目录名；脚本拒绝覆盖证据。workload/probe 的语义、参数序列和读回口径冻结，
框架接入变化位于 framework_bridge；mixed 标签和相应 shader 分组分支明确单独报告。

## 生命周期与迁移检查

| 验收项 | 对应实测或代码检查 | 当前结果 |
|---|---|---|
| 未提交注销、延迟 fence、GT release，F=1/2/3 | RendererRegistry.AdmittedExitWaitsForSubmissionFenceAndHostReleaseAcrossFlightCounts，双后端 | 通过 |
| 跨宿主完成不替代、编译失败仍保留使用 | RendererRegistry.ForeignHostAndFailedCompileCannotReleaseAcceptedWork | 通过 |
| 替换版本、过期 ID、generation 复用、混合组拒绝 | RendererRegistry.MaterialReplacementPreservesAcceptedVersionAndRejectsMixedDomains | 通过 |
| 兼容组、foreign BindingHandle、值判等 | RendererRegistry.CompatibleLayoutsReuseFinalValuesAndRejectForeignBindingHandles | 通过 |
| 内存压力清理闲置并重试，活跃项保留 | RendererRegistry.PressureEvictsIdleStorageAndRetriesWithoutEvictingActiveUses | 通过 |
| 常量 arena、parent/view/frame/登记依赖 | RendererRegistry 的 EqualBytesShareRanges 与 TextureExit 测试 | 通过 |
| 两个 Scene 共用外部资产，真正完成及退出确认后释放 | RendererRegistry.SharedSceneAssetExitRequiresRendererConfirmationAfterTrueCompletion | 通过 |
| Plan 在收集时不准备，统一 Compile，完成前保持所有权 | RendererRegistry.FrameOwnsAllPreparationsAndCompilesOnlyAfterCollection | 通过 |
| Color/Depth、手动/自动实例、剔除/排序、重连、服务重建 | RasterDraw、SceneDraw 双后端 GPU 读回 | 通过 |
| 输出丢弃、无 Scene producer、单/双线程 | PipelineOutput、SceneDraw、RuntimeServices、GpuSystemAttachment | 通过 |
| CPU-only 场景交付、实例 schema/参数与资产退休 | SceneUpdates、SceneAssets、SceneGpu 等 runtime 回归 | 通过 |
| framework 内共享所有权和旧执行路径 | 全目录搜索 shared_ptr/weak_ptr/enable_shared_from_this/make_shared/intrusive_ptr 及旧类型 | 零匹配 |

全目标 Debug 构建成功，包含全部 examples、benchmarks 和测试。首轮 runtime 完整运行发现同一个批次测试的两个后端失败：
测试迁移后在同一帧重复 Compile，拒绝后跳过了完成清理；修正为只在未编译时调用 Compile。
同时收尾内部资源退出确认积累问题。随后 43 项定向回归全部通过，日志保存在 `build_descriptor_reuse/final-targeted-01.log`。
这段过程不改变基线数据，也不把早期失败计为最终通过。

最终源码完整运行 runtime 488 项、render 153 项，构建与测试串行。原始计数及复测分别保存在
[tests.json](2026-10-16-render-framework-refactor-tests.json)，包含每项结果、命令以及日志/JUnit 校验值：

| 运行 | 通过 | 失败 | 跳过 |
|---|---:|---:|---:|
| runtime 完整运行 | 482 | 1 | 5 |
| 输入停顿用例，原代码复测 4 项各 5 次 | 20 次执行 | 0 | 0 |
| runtime 合并上述复测后的用例结果 | 483 | 0 | 5 |
| render 完整运行 | 152 | 0 | 1 |

完整 runtime 的唯一失败是 `RuntimeFoundation.VulkanSingleThreadInputDuringGpuStall`：事件序列开头多出测试
没有发送的 `U+0013` 字符，之后全部注入事件顺序正确，额外字符使 27 项变成 28 项。根据日志推测为桌面输入干扰，
未修改断言或产品代码；两后端、单/双线程的对应 4 项各连续复测 5 次通过。保留首次失败输出，不将原始运行改写为全绿。

5 个 runtime 跳过项都是已有 Vulkan 多交换链 validation 问题：当前 1.4.357 版本尚无 1.4.364 的修复，
涉及 OrderedSubmissions、SwapChainLifecycle 的单/双线程及 MultiSwapChainHostWaitBeforePresent。
render 的 1 项跳过是 Vulkan 原生缓存专用测试被参数化为 D3D12 时不适用。
这些跳过不计为通过；42 项核心离屏读回和 Renderer 生命周期双后端验收没有跳过。
环境限制说明见[构建与测试](../guide/build-test.md#vulkan-多交换链同步验证已知问题)。

## Release 性能证据

使用冻结基线源码与最终源码分别构建相同 Release 配置：Ninja、MSVC、profiler OFF、benchmark ON，
test/example/shader-tool OFF，D3D12/Vulkan/JIT/mimalloc 保持启用。基线恢复自正式采样 source.zip，包含原 sampler 改动，
third_party/SDKs 通过本机目录连接使用相同依赖，不修改依赖源码。

比较采用现有 Google Benchmark：MaterialScene 的 N1000、M1/100、V3、dirty0、F3，分别测 PrepareRecord 和 FullFrame；
SceneGpu 的 F3、V3、changed0/100 测 PrepareAndRecord。按 before/after/after/before 独立进程串行运行，
每个 case 每轮 3 次，最终每版本 6 次。MaterialScene 每次最短 0.2s；SceneGpu 每次固定 5000 次迭代，
避免亚微秒 case 的自动校准产生过长运行。两版本使用同一方法。只比较同名计时范围，不以 descriptor 样例推导帧率。
MaterialScene 的绑定 ABI 随框架一起迁移，场景和绘制工作量保持；这是完整架构对比，不是 shader ABI 不变的单因素实验。

最终 8 个进程均成功，12 种负载、每版本各 6 个样本均无 benchmark error。
[performance.json](2026-10-16-render-framework-refactor-performance.json) 保存原始样本、完整命令、exe SHA-256、构建选项及配置校验，
[performance.csv](2026-10-16-render-framework-refactor-performance.csv) 保存中位数配对。以下为 wall time 微秒，增幅由未四舍五入值计算：

| 负载 | D3D12 前 → 后 | 变化 | Vulkan 前 → 后 | 变化 |
|---|---:|---:|---:|---:|
| MaterialScene PrepareRecord，M1 | 227.21 → 249.40 | +9.8% | 216.14 → 234.91 | +8.7% |
| MaterialScene PrepareRecord，M100 | 695.15 → 925.52 | +33.1% | 716.88 → 920.77 | +28.4% |
| MaterialScene FullFrame，M1 | 265.63 → 293.33 | +10.4% | 261.04 → 274.21 | +5.0% |
| MaterialScene FullFrame，M100 | 741.18 → 1029.94 | +39.0% | 821.80 → 1012.15 | +23.2% |
| SceneGpu changed0 | 0.700 → 1.218 | +0.519 µs | 0.562 → 1.087 | +0.525 µs |
| SceneGpu changed100 | 49.41 → 50.20 | +1.6% | 17.46 → 18.60 | +6.5% |

descriptor 目标达成，但 CPU 时间没有改善。最终数据仍显示完整帧登记、使用清单与编译路径的成本；
这里不把未通过 profiler 分解的各项成本归因当作实测结论。
后续使用独立 Google Benchmark fixture 完成了[回退原因定位](2026-10-16-render-framework-regression-diagnosis.md)，
区分统一编译、录制与排除 fence 等待的完成清理，并保留了固定最终绑定值的规模控制实验；生产代码未随该分析修改。
大多数 6 样本集合的变异系数为 0.7%–5.2%；Vulkan 基线 M100 的 PrepareRecord/FullFrame 分别为 20.6%/16.7%，
原始范围为 705.29–1106.72 µs / 751.42–1157.12 µs，波动明显。中位数不等于统计显著性或跨设备保证。

首轮性能运行暴露更严重的回退：M100 PrepareRecord 的 D3D12/Vulkan 分别为基线的 3.90×/2.53×。
该轮 [performance-initial.json](2026-10-16-render-framework-refactor-performance-initial.json) 和
[CSV](2026-10-16-render-framework-refactor-performance-initial.csv) 同样保留，不替换成最终结果。
随后在整个 frame 内合并相同组请求与 PSO 解析、缓存可见 CPU 材质查找、改用整块字节 hash 后完整判等，
并让连续 draw 录制只比较编译好的 ID/状态以省去重复绑定。没有把参数创建、上传或内容去重移到 Record。
最终 42 项离屏和完整回归均在这些优化完成后运行。

构建目录是 `build_descriptor_reuse/perf-before/` 与 `build_descriptor_reuse/perf-after-optimized/`；
首轮未优化程序保留在 `build_descriptor_reuse/perf-after/`，原始日志与命令位于 `performance-final-01/` 和 `performance-final-02/`。
Release 配置命令如下，在 VS x64 开发环境执行；`<source>` 分别是冻结基线源码目录和仓库根，`<build>` 对应上面的两个目录：

```powershell
cmake -S <source> -B <build> -G Ninja -DCMAKE_BUILD_TYPE=Release -DRADRAY_BUILD_TESTS=OFF -DRADRAY_BUILD_EXAMPLES=OFF -DRADRAY_BUILD_SHADER_TOOLS=OFF -DRADRAY_BUILD_BENCHMARKS=ON -DRADRAY_ENABLE_PROFILER=OFF
cmake --build <build> --target bench_material_scene bench_scene_gpu --parallel 4
```

本机新 Release 配置的 MSVC include 前缀曾因中文编码检测错误而令 Ninja 漏记依赖；修正生成的编译器 metadata 前缀后，
已重新生成并实际重编包含最终 Renderer、资源及 SceneDraw 的 unity 单元，核对依赖记录和 exe 校验，再开始最终测量。
Debug 完整构建没有此问题。具体构建日志校验在 tests.json，不能以一次 stale “no work to do” 作为构建证据。

## 未通过及未运行项

- CPU 性能无回退没有达成，具体幅度如上。本次计划没有设置 CPU 时间硬阈值，但该回退是需要保留的后续优化事项。
- 5 个 Vulkan 多交换链用例受本机 validation 版本限制未运行；1 个 render 参数化项不适用于 D3D12。未将其算入通过数。
- 输入停顿测试首次失败及无代码改动的 20 次复测均保留。额外字符的外部来源是推断，未额外采集系统输入追踪。
- 其它 GPU、操作系统及小时级稳定性没有运行；没有从离屏分配计数推导 FPS。

## 边界

实际测量仅代表本机 RTX 3060、Windows 和本报告负载。未运行其他 GPU、操作系统或小时级稳定性测试。
本次共享单位是后端兼容的完整参数组；不同完整组之间局部 D3D12 table 共享仍不在范围内。
shader 服务及原生布局必须活到 Renderer 排空并 Shutdown；独立宿主遵循同样的真实完成与退出确认协议。
