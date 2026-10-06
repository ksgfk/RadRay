> - 适用: 诊断 `examples/scene_sync` 在 `--instances=100000` 时的稳态性能瓶颈与优化顺序
> - 权威: 2026-10-06 本机实测快照；诊断已完成，优化未实施，不作为跨机器性能契约
> - 锚点: `examples/scene_sync/example_scene_sync.cpp`, `examples/scene_sync/scene_draw.cpp`, `modules/runtime/src/render_scene/scene_gpu.cpp`, `modules/runtime/src/gpu_resource.cpp`, `modules/render/src/d3d12/d3d12_impl.cpp`, `benchmarks/bench_scene_gpu/bench_scene_gpu.cpp`

# scene_sync：100,000 对象性能诊断

## 结论

默认 **D3D12、Release、单线程、F=2、单视图** 下，瓶颈在 **CPU 渲染命令录制**。
最大的两个成本是大量小范围上传的 copy 命令，以及逐对象的绘制与资源绑定。

- 每帧更新 **33,334** 个对象，拆成 **33,334 个上传范围**。其中仅 copy 命令录制就花 **6.77 ms**，占实测帧间隔约 **42.6%**。
- 每视图发出 **100,000 次 indexed draw**，`SceneDraw::Draw` 花 **6.18 ms**，占约 **38.9%**。
- GPU 上传约 **0.43 ms**，GPU 绘制约 **3.54 ms**，GPU 整帧约 **4.02 ms**。
- 三次重复采样的平均帧间隔为 **15.993 ms**，折算约 **62.5 FPS**。双线程没有明显改善；Vulkan 将主要成本转移到绘制录制，总帧间隔仍约 16 ms。

优化优先顺序：**合并上传范围 → 按共享网格等条件批量绘制/instancing → 再优化变换求值与 bounds**。
本次只保留诊断插桩，未修改上传策略或绘制方式。

## 基线与采样方法

| 项目 | 配置 |
|---|---|
| Git 基线 | `2aaa68e085eae7c44b2706ae74fc33084e4b405a`，叠加任务开始时的工作树修改 |
| 原有修改 | `example_scene_sync.cpp/.h` 的相机输入与 FPS 标题、`docs/guide/build-test.md`；均保留 |
| 系统 | Windows 11，10.0.26300；高性能电源计划 |
| CPU / 内存 | i7-13700K，16 核 / 24 线程；系统可见物理内存约 31.7 GiB |
| GPU | RTX 4080；两后端均由启动日志确认选择该显卡；驱动 `32.0.16.1714` |
| 构建 | Release，VS 18 2026 ClangCL，clang 22.1.3；沿用工程 SIMD/LTO 配置 |
| 运行 | 1280×720 窗口、Immediate Present、F=2、单视图、验证层关闭；不操作相机 |
| 采集 | Tracy 0.14.1 / `30997d5`，profiler 开启、采集端连接 localhost；进程串行运行 |

共采集 **20 份 Tracy trace**。原始代码先采样，再补充粗粒度插桩并重复采样。
原始 D3D12 两轮平均帧间隔为 15.762 / 15.653 ms；增加 CPU zones、plots 和 GPU debug groups 后，
三轮为 15.928 / 16.088 / 15.962 ms，均值间 CV 为 0.53%。这组顺序采样显示配置开销与运行波动合计约几个百分点，不能当作严格交替的插桩开销实验。

窗口样例丢弃最初 120 次 Render 回调，排除 shader JIT、PSO、初始全量同步和物理 buffer 初始化；
区间结束在倒数第六次 Render 的开始，避免尾部关停。通常保留 473 次完整 Render 回调、472 个帧间隔。
1k / 10k 最终复测分别保留 9,873 / 2,873 次回调。stress 负载自身预热 120 帧，随后测 600 帧。
构建、正确性测试、trace 采集和 benchmark 分开进行。

CPU 数据来自 Tracy 原生 `csvexport -u` 事件。按线程重建嵌套关系，校验区间包含关系及非负 self time，
然后以稳态 Render 回调数归一化。它测量代码区间的墙钟时间，可能包括驱动调用与调度，不能解释为 OS 纯运行时间。
帧间隔是相邻 Render 开始时间之差，包含两次绘制间的 CPU 工作和等待；FPS 为其均值的倒数，
不是逐帧端到端延迟。P95 是 trace 中帧间隔的分位数。

GPU 分段来自后端 debug group 时间戳。`SceneSync/LastResolvedGpuMs` 使用现有 flight timestamp query，
读数滞后当前 CPU 帧，不能逐帧配对。CPU 与 GPU 可以重叠，不能将二者耗时相加作为帧时间。
本报告的窗口数字用于诊断；正式微基准另使用 Google Benchmark 的原生 reporter。
未测 Debug、开启验证层或其他机器配置。

## 原场景为什么会放大成本

`--instances` 控制的是 Actor / StaticMeshComponent 数量。所有对象共享同一立方体资产，
但 `scene_draw.cpp` 的 `DrawIndexed(..., 1, ...)` 每次只画一个对象；并未使用 GPU instancing。
单视图对应 100,000 次 draw，三视图对应 300,000 次。样例没有深度测试或可见性剔除。

`example_scene_sync.cpp` 在创建时将 `i % 3 == 0` 的对象挂到同一父节点，
每帧用 sin/cos 修改父节点位置。父节点的局部变化在 RT `SceneTransform::Evaluate` 中传播，
最终改变 33,334 个 mesh 的世界矩阵与 bounds。GT 只修改少量局部状态，所以 `Update` 很短。

这些 mesh 的 ShapeId 槽位为 0、3、6……。
`SceneGpuData::Prepare` 只把相邻槽位合成一个上传范围，因此每个变化对象各形成一段 **64 B** 上传：
每帧共 **2,133,376 B（2.03 MiB）**、33,334 个范围。
`ResourceUploader::TryUploadBufferRanges` 对每个范围预留 staging，并单独调用一次 `CopyBufferToBuffer`。
D3D12 实现直接转发到 `ID3D12GraphicsCommandList::CopyBufferRegion`。

F=2 的每个物理 buffer 都累计待上传变化；本样例始终修改同一组子对象，因此稳定为 33,334 条，
不会因双 flight 翻倍。三视图共享同一个 Scene / flight buffer，本帧重复 Prepare 会直接复用。
另外，已有 `SceneTransforms` plot 统计的是旧式 `batch.Transforms`，其值为 0 不代表本帧没有局部变换更新或层级传播。

## CPU 与 GPU 时间

下表为增加最终细分插桩后的 `detail_d3d12_100k` / `detail_vulkan_100k` 区间，单位 ms。
缩进关系用“其中”表达；父区间已包含子区间，不能重复相加。

| 阶段 | D3D12 | Vulkan | 含义 |
|---|---:|---:|---|
| 平均帧间隔 / P95 | 15.874 / 16.372 | 16.365 / 16.763 | 窗口绘制节奏 |
| `SceneGpuData::Prepare` | 7.793 | 2.032 | 排序、打包、上传准备与录制 |
| 其中 `TryUploadBufferRanges` | 7.202 | 1.455 | staging 与 copy 命令 |
| 其中 `StageBufferRanges` | 0.410 | 0.397 | 预留、memcpy、收集 source barriers |
| 其中 `RecordBufferCopies` | **6.768** | 1.047 | 33,334 次 RHI copy 调用的录制循环 |
| `SceneDraw::Draw` | **6.182** | **12.149** | 100,000 次对象绑定和绘制 |
| `ConsumeRenderUpdates` | 1.327 | 1.331 | Apply 及每 flight GPU pending 集合维护 |
| 其中 `RenderScene::Apply` | 1.247 | 1.253 | 层级求值、bounds 与变化集合整理 |
| 其中 `SceneTransform::Evaluate` | 0.614 | 0.609 | 受影响子树求值 |
| `Submit` | 0.447 | 0.674 | host writes flush、队列提交、Present 等 |
| `Update` | 0.007 | 0.007 | GT 更新、收集与 Seal |
| `WaitFlightFence` | 0.0002 | 0.0008 | 单线程稳态 fence 等待很小 |
| GPU 上传 / 绘制 | 0.431 / 3.541 | 0.266 / 7.323 | 后端时间戳区间 |
| GPU 整帧 plot | 4.015 | 7.606 | 最近一次完成 flight 的解析值 |

D3D12 uploader 内约 **94%** 的时间落在 copy 命令录制循环。
Prepare 除去 uploader 的 self time 为 0.592 ms，包含排序、矩阵打包和集合维护，
因此当前不应优先优化 allocator、memcpy 或排序。GPU 整帧明显短于 CPU 帧间隔，
单线程 fence 等待接近零，证明本配置的主限制在 CPU。

Vulkan 的 copy 录制更便宜，但逐对象绘制录制约 12.15 ms，占帧间隔约 74%。
因此两个后端的首要子瓶颈不同，单纯切换后端没有解决大量命令的问题。

`SceneDraw::Draw` 的 zone 覆盖资源解析、PSO / parameter set / VB / IB 绑定、push constants 和 draw；
本次没有把它拆到每个 API，不能断言其中某一种绑定调用独占全部时间。
代码还按 `i % 7 == 0` 使用负缩放，正反剔除 PSO 在对象顺序中频繁交替，进一步阻碍批量绘制。

## 对照证据

| 窗口配置 | 平均帧间隔 ms | CPU Prepare ms | CPU 绘制相关录制 ms |
|---|---:|---:|---:|
| D3D12，1k，最终插桩 | 0.423 | 0.090 | 0.070 |
| D3D12，10k，最终插桩 | 1.800 | 0.775 | 0.651 |
| D3D12，100k，最终插桩 | 15.874 | 7.793 | 6.182 |
| D3D12，100k，双线程，原始插桩 | 15.749 | 7.821 | 约 6.177 |
| D3D12，100k，三视图，原始插桩 | 28.188 | 7.868 | 约 18.490 |

末两行没有独立 Draw zone，绘制相关时间用 Render 减去 Prepare，包含少量窗口与 pass 维护，
属于绘制录制的近似上界。双线程仍由 RT 的 Apply、Prepare 和 Draw 串行链限制；
GT fence / writable-slot 等待是背压表现，不宜据此判断 GPU 成为主瓶颈。
三视图的上传准备基本不变，绘制相关录制约增至三倍，与代码行为一致。

复用相同 `SceneDraw` 的离屏 stress 辅助负载给出以下诊断数据，均为 D3D12、100k、单线程、F=2、单视图：

| 负载 | 实际上传对象 / 范围 / 字节 | Prepare ms | uploader ms | 说明 |
|---|---|---:|---:|---|
| upload + idle | 0 / 0 / 0 | 0.0001 | 无上传调用 | 常驻 100k 并不自动触发全量上传 |
| upload + move，changes=33334 | 66,668 / 约 33,332 / 4,266,752 B | 9.088 | 7.215 | 轮转修改，F=2 累积两个不同帧的对象集合 |
| upload + parent | 100,000 / 1 / 6,400,000 B | 1.274 | 0.313 | 全部子对象连续，合成一个范围 |

连续范围负载上传的字节更多，但 uploader 用时大幅下降，支持“范围数 / 命令数”是主要放大因素。
这些负载的变换选择与窗口场景不同，不能将总帧时间直接相减当作同场景优化收益。
另测得静止 100k 的离屏 draw 负载：CPU `SceneDraw::Draw` 约 5.99 ms，
无对象上传；说明绘制录制成本独立存在。

作为独立微基准，`bench_scene_gpu` 固定 10k、F=2、views=1，预热 0.1 s、min-time 0.2 s、重复三次，
采样期间没有连接 Tracy viewer。下表为 Google Benchmark 原生 `Time` 的 repetition mean，单位 µs：

| `PrepareAndRecord` | D3D12 | Vulkan | 上传范围 / 字节 |
|---|---:|---:|---|
| changed:100 | 33.617 | 13.999 | 100 / 6,400 B |
| changed:10000 | 109.633 | 130.005 | 1 / 640,000 B |
| parent | 91.388 | 126.381 | 1 / 640,000 B |

它排除 GT 修改、Apply、提交和等待，也不绘制 geometry；这组 10k 数据不能当作 100k 窗口帧率。
数据同时说明，在范围很少时仍有随对象数量增长的打包成本，不能把所有上传工作都归因于 copy API。

## 优化建议与验收方向

1. **先减少上传范围。** 当前 33% 槽位变化却拆成 33,334 条命令。可对高密度变化采用整段 / 全 buffer 上传，
   或合并相近槽位并补齐间隙对象的当前矩阵。全量 100k 仅 6.4 MB，可作为诊断候选。
   目标是把 copy 数从 33,334 降到少量；保留每 flight pending、失败重试、generation 和 buffer 生命周期语义。
   上述连续范围对照支持这一方向，但还未对原场景实施并测得收益。
2. **把共享立方体转成批量绘制。** 按 mesh、section、材质和 ReverseCulling 等条件分组，使用 instancing，
   为 instance 提供 ObjectSlot 映射，并在组级绑定共享参数和几何。当前单 cube 很适合验证这一改动。
   同时评估 PSO 分组以减少正反剔除切换；当前没有深度测试，重排对象可能改变重叠处的覆盖顺序，需保持图像语义。
   目标是减少 100,000 次绑定 / push / draw 的重复成本；两后端、负缩放和多视图均需验证。
3. **再处理 Apply 和矩阵 / bounds。** 这部分约 1.25 ms，层级求值约 0.61 ms，优先级低于两个录制循环。
   父节点确实在移动，其子对象更新是必要工作。后续可验证更好的批量布局或并行求值，不能通过漏更新获得“优化”。

后续正式比较应扩展 Google Benchmark，覆盖准确的 100k / 每三个槽位变化、静止 draw 和批量 draw，
固定相同二进制配置、viewer 连接状态、flight 和后端。复测时同时验收 copy 数、draw 数、CPU/GPU 时间与图像结果。
本报告未给出优化后的 FPS 承诺。

## 保留的插桩与验证

按用户要求保留插桩，未增加逐对象采样或统计字段：

- CPU zones：`SceneDraw::Draw`、`ResourceUploader::TryUploadBufferRanges`、`StageBufferRanges`、`RecordBufferCopies`。
- GPU debug groups：`ObjectUploadsGPU`、`SceneSyncDrawGPU`。
- Plots：`SceneSync/LastResolvedGpuMs`、`SceneGpu/UploadObjects`、`SceneGpu/UploadRanges`；空闲帧明确采样 0 上传。

使用项目 `radray/profiler.h` 宏和现有 RHI debug group API，GPU 时间戳仍由 render 后端实现。
使用方法已同步至[构建与测试](../guide/build-test.md#生命周期与增量渲染验收)。

最终 Release 目标构建成功。设置 `RADRAY_TEST_REQUIRED_BACKENDS=d3d12,vulkan` 后，
31 项相关 GTest 全部通过，覆盖 SceneSyncCorrectness、SceneApplyChanges、SceneViews、SceneDraw，
以及双后端 SceneGpu 读回、flight、generation、失败恢复、稀疏范围和生命周期。
六个 Google Benchmark 场景各三次 repetition 均有原生结果且没有 `error_occurred`。

## 复现与原始数据

```powershell
cmake --build build_release --config Release --target example_scene_sync example_framework_stress test_radray_runtime bench_scene_gpu --parallel 8
$env:RADRAY_TEST_REQUIRED_BACKENDS = 'd3d12,vulkan'
ctest --test-dir build_release -C Release -R 'SceneSync|SceneApplyChanges|SceneViews|SceneDraw|SceneGpu|SceneTransform|WorldTransform' --output-on-failure
# 每次只启动一个采样进程；预先启动匹配版本的 Tracy capture，连接 127.0.0.1。
build_release/_build/Release/example_scene_sync.exe --d3d12 --instances=100000 --frames=600
build_release/_build/Release/example_scene_sync.exe --vulkan --instances=100000 --frames=600
build_release/_build/Release/example_scene_sync.exe --d3d12 --instances=100000 --multithread --frames=600
build_release/_build/Release/example_scene_sync.exe --d3d12 --instances=100000 --views=3 --frames=600
build_release/_build/Release/bench_scene_gpu.exe '--benchmark_filter=SceneGpu/(d3d12|vulkan)/PrepareAndRecord/F2/(changed:(100|10000)|parent)/views:1/real_time$' --benchmark_min_time=0.2s --benchmark_min_warmup_time=0.1 --benchmark_repetitions=3 --benchmark_out=scene_gpu_100k_diagnostic.json --benchmark_out_format=json
```

本机原始证据保存在 `artifacts/scene_sync_100k_20261006/`（该目录被 Git 忽略）：

- 每个场景的 `capture.tracy`、原生 `cpu.csv` / `gpu.csv` / `plots.csv`、日志、命令与稳态分析 JSON。
- `profile.py`：采集、导出与区间分析；本机 CLI 路径为 `F:/program/tracy/`，重新采集应使用新的输出目录。
- `baseline.patch`、`diagnostic-source.patch`、`final-source.patch`；原始四个源码备份、CMakeCache、三阶段二进制 SHA-256。
- `tests-final.log`、`benchmark.json` / `benchmark.log` / `benchmark-summary.json`、`details-summary.json` 和校验元数据。

窗口原始采样对应 `d3d12_*` / `vulkan_100k`；粗分段复测对应 `diag_*`；
细分 uploader 后的最终采样对应 `detail_*`。关键数字均已写入本文，阅读结论无需依赖这些本机采样文件。
