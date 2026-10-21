> - 适用: 选择 CPU/GPU 基准、定位帧成本、比较改动或解读历史性能报告
> - 权威: 当前测量入口与结果解释；构建和正确性回归见 build-test，历史数值只属于各报告基线
> - 锚点: `benchmarks/CMakeLists.txt`, `benchmarks/bench_scene_sync/bench_scene_sync.cpp`, `benchmarks/bench_lifecycle/bench_lifecycle.cpp`, `benchmarks/bench_scene_gpu/bench_scene_gpu.cpp`, `tools/run_scene_sync_benchmark.py`, `tools/run_gpu_lifecycle_benchmark.py`, `tools/run_material_scene_benchmark.py`

# 性能测量

先按[构建与测试](build-test.md)恢复依赖并完成相关正确性回归。性能目标由
`RADRAY_BUILD_BENCHMARKS` 开启，使用 Release；构建、测试、benchmark 和 trace 采集串行执行。
本页从临时测量中保留可复用的方法与限制，不把旧机器上的数值作为当前性能指标。

## 选择入口

| 要回答的问题 | 入口 | 测量边界 |
|---|---|---|
| World 到 CPU Scene 的交付成本 | `bench_scene_sync` | 单/双线程同步吞吐，不含 GPU 或 draw |
| 创建、删除与生命周期帧成本 | `bench_lifecycle` | CPU owner、S1、交付和回收 |
| Scene GPU 参数更新成本 | `bench_scene_gpu` | CpuSync、PrepareAndRecord、SubmitFrame；不画 geometry |
| 实际材质与自动/手动实例绘制 | `bench_material_scene` | 真实离屏绘制；需要 JIT |
| 命令 storage、附件、提交与窗口 | `bench_command_allocator` | 各 family 的 CPU/GPU 范围不同，见下文 |
| descriptor Set/Flush 与分配 | `bench_d3d12_descriptor` | CPU，不提交 GPU |
| graph、bounds 与排序算法 | `bench_raster_prepare` | CPU，不创建 command 或提交 GPU |
| OBJ 文本解析 | `bench_read_obj` | 内存文本解析，不含磁盘 IO |
| 真实 runner 的持续帧跟踪 | `example_framework_stress`、`example_scene_sync` | Tracy 诊断；正式数值对照另用 benchmark |

先用可执行文件的 `--benchmark_list_tests=true` 核对当前 case 名，再用完整 filter 选取负载。
目录和目标名不等于 case 名；Google Benchmark 自带 reporter 后缀也属于名称的一部分。

## 比较与证据口径

基线记录提交、未提交补丁、负载源码、构建配置、完整命令及二进制 SHA-256。基线带工作树修改时明确说明，
不能用纯净提交代替。前后固定机器、编译器、allocator、profiler/validation、flight、视图、材质分布和脏比例；
交替运行独立进程，区分一次覆盖扫描与多轮配对比较。脚本选项不等于结果已经可比，仍需核对实际匹配的 case。

初始化、JIT、PSO、首次 buffer 扩容与稳态分开；Tracy 用对应 workload 的 warmup complete message 切掉冷区间。
持续更新必须实际改变值，不能让 setter 的同值短路悄悄降低脏比例。重点同时覆盖静止、单次修改后静止、
稀疏/密集、随机/顺序、父节点传播、增删和多 flight；仅凭 dirty 百分比不能预测局部性或 GT/RT 重叠收益。

| 指标 | 可以说明什么 | 不能代替什么 |
|---|---|---|
| CPU zone 墙钟时间 | 该阶段含驱动调用、调度在内的经过时间 | OS 纯 CPU 执行时间 |
| 进程 CPU 时间 | GT/RT 等线程的 CPU 时间合计 | 墙钟延迟或 GPU 时间 |
| repetition 中位数/CV | 多次测量结果及波动 | 单帧 P95/P99 |
| GPU timestamp | 所标记 GPU 区间的时长 | CPU 总成本或画面显示时间 |
| frame completion latency | 同一 frame 身份从开始到真实完成 | FPS 或 Present 返回时刻 |
| host/upload bytes | 本次提交的主机写入或上传量 | PCIe 总流量、显存驻留 |
| `scene_buffer_bytes` | 所有 flight 的实际 buffer 分配容量 | 单份 Scene 大小或 driver residency |
| native allocation/block bytes | D3D12MA/VMA 的占用 allocation/预留 block | opaque RP/FB/descriptor 内存或 OS 驻留 |
| private bytes / working set | 进程内存曲线 | GPU 对象大小或显存 |

CPU 与 GPU 可以重叠，不把二者相加得到 FPS。短测的百分比、单轮覆盖以及亚微秒差异不能证明稳定收益；
分别检查 CPU、GPU 和尾部延迟，对疑似退化做独立长采样。预算拒绝、skip、error 与成功运行分列，
不把未运行 case 算作通过。drain 后缓存仍可能保留物理存储，回收正确性应检查使用引用和 owner 时点。

历史设计与实测数据见[架构地图的历史资料](../architecture/overview.md#历史资料)。其中未入库的
`build*/` 日志、trace、源码归档仅是采样时的本机证据；文件缺失时需要重新测量，不能从报告重新生成原始样本。

## 性能采样（Tracy）

`RADRAY_ENABLE_PROFILER` 默认 ON，保留 Tracy client、CPU frame/phase zones 和后端 GPU 时间戳。
viewer 版本应与 project_manifest.json 的 Tracy tag 匹配；client 只监听 localhost、按需连接。
业务插桩只用 radray/profiler.h 宏，详见 [Core](../architecture/core-facilities.md#性能采样宏)。
CPU record/Submit 时间与 GPU 时间线分开解读。关闭使用 `-DRADRAY_ENABLE_PROFILER=OFF`。
旧 RenderGraph/Forward 的 profile/record harness、逐命令采样开关及 RG plots 已移除。

### 基础框架 Tracy 压测样例

`example_framework_stress` 使用真实 Application GPU runner、World/Actor/StaticMeshComponent 和共享 Ready
立方体资产，复用 `scene_sync` 的绘制实现。它是持续运行的抓取样例，性能数字的正式对比仍使用 Google Benchmark。
默认 D3D12、双线程、F=2、10,000 对象、每帧修改 100 个对象、单视图；先预热 120 帧，再运行 60 秒。
默认没有窗口和交换链，draw 档绘制到每 flight 独立的 1280×720 离屏目标，避免 Present/vsync 限制吞吐。

示例与 runtime 一样通过 `radray_optimize_flags_binary` / `radray_optimize_flags_library` 使用 Release SIMD 配置，
保证跨 target 传递的 Eigen 矩阵及 `SceneViewRequest` 对齐和数组步长一致。

Windows 可用独立 Release 目录构建；已有多配置目录也可直接构建 Release 目标：

```powershell
cmake --preset win-x64-release-clangcl -B build_profile -DRADRAY_BUILD_EXAMPLES=ON -DRADRAY_ENABLE_PROFILER=ON
cmake --build build_profile --config Release --target example_framework_stress --parallel 12
build_profile/_build/Release/example_framework_stress.exe --mode=upload --workload=move --objects=10000 --changes=100 --seconds=120
```

在匹配版本的 Tracy viewer 中手动连接 `127.0.0.1`；本项目关闭自动广播。`--seconds=0` 持续运行，
`--frames=N` 改为预热后运行 N 个 GT 负载帧，优先于 seconds；`--warmup=N` 调整预热帧数。
退出沿用 runner 的关停协议，双线程末尾少量已发布帧可能只消费同步包而不再调用绘制；日志分别报告 GT 更新与渲染回调次数。
GT/RT 分别发出 warmup complete 消息，`Stress/Warmup` plot 为 0 的渲染帧属于正式运行。
连接晚于预热仍可抓取后续稳态；预热不足时应排除首次同步、shader 编译、PSO 创建和 buffer 扩容。

| `--mode` | 执行范围 |
|---|---|
| `world` | 创建并更新对象，但 World 不连接 renderer；仍运行相同 GPU runner 和空场景交付，作为无场景连接的参照 |
| `sync` | 再连接 World，捕获视图并交付/应用 CPU RenderScene 更新，不准备对象 GPU buffer |
| `upload` | 在 sync 上通过 RendererScenes 准备 GPU 镜像，录制和提交对象参数上传，不发出 geometry draw |
| `draw` | 通过 UnlitRenderPipeline/SceneDraw 剔除、合批并录制无光照绘制；负缩放按 winding 拆批，无深度测试 |

| `--workload` | 每帧负载 |
|---|---|
| `idle` | 对象与相机完全静止，无 Actor Tick；观察随常驻对象数增长的空闲成本 |
| `move` | 修改均匀分布的 `--changes=N` 个对象，选择随帧轮转；每次翻转局部 Z，保证值发生变化 |
| `parent` | 全部对象挂到同一个父节点，每帧只改父节点一次；观察通知、层级求值和上传的传播成本；忽略 changes |
| `tick` | 为前 `--changes=N` 个 Actor 开启空 Tick hook，对象保持静止；与 idle 比较调度成本 |
| `churn` | 每帧请求销毁并创建 `--changes=N` 个对象，保持存活总数；新对象仍使用同一 Ready 资产 |

`--changes` 上限为 objects，允许 0；`--objects=0 --workload=idle` 是空场景参照，仍含一个 World 和相机。
其他配置包括 `--d3d12`/`--vulkan`、`--single-thread`/`--multithread`、`--flights=1|2|3|4`、`--views=N`（正整数）。
`--window` 增加窗口与 Immediate 呈现；world/sync/upload 档仅清屏，draw 档显示对象，窗口维护和呈现成本也会进入捕获。
`--validation` 仅用于正确性排查，正式抓取默认关闭；`--no-gpu-profiler` 可关闭帧 GPU 时间戳，CPU zones 保留。
完整参数见 `--help`。启动日志记录实际配置，热路径不逐帧打印，也不做逐对象校验或逐对象 Tracy 插桩。

建议固定后端、线程、flight、viewer 连接状态和 profiler 开关，分别抓取以下配置；每次只启动一个进程：

```powershell
$stress = 'build_profile/_build/Release/example_framework_stress.exe'
& $stress --mode=sync --workload=idle --objects=0 --seconds=120
& $stress --mode=sync --workload=idle --objects=100000 --seconds=120
& $stress --mode=world --workload=move --objects=100000 --changes=100 --seconds=120
& $stress --mode=sync --workload=move --objects=100000 --changes=100 --seconds=120
& $stress --mode=upload --workload=move --objects=100000 --changes=100 --seconds=120
& $stress --mode=upload --workload=parent --objects=100000 --seconds=120
& $stress --mode=sync --workload=tick --objects=100000 --changes=100000 --seconds=120
& $stress --mode=upload --workload=churn --objects=100000 --changes=100 --seconds=120
& $stress --mode=draw --workload=move --objects=10000 --changes=100 --window --seconds=120
```

Tracy 中从 `Stress::Update`（样例制造负载）与 `World::Tick` 开始，继续看
`Application::FinalizeWorldAndSealGT` 内的生命周期、通知、Collect 和 Seal，
RT 看 `SceneManager::ConsumeRenderUpdates` / `RenderScene::Apply`、`Stress::PrepareObjects` /
`SceneGpuData::Prepare`、`Stress::DrawObjects`，提交看 `GpuSystem::SubmitFrame`。
`Application::ServiceFrameBoundaryGT` 包含完成回收、资产和 scheduler 泵。
`PrepareFrame` 现在是非阻塞检查与 S0；`ApplicationLoop::WaitProgress` 等待消息或 CPU/排空进度，
`WaitReadySlot` 是 RT 等待发布，`GpuSystem::WaitFlightFence` 是 GT 的有限 GPU 等待片段。
应展开查看，不能将等待时间归因于框架执行成本。比较旧 runner 时优先使用共同的 S0、Update、RenderFrame
及提交 zones，同时查看 GT 开始间隔、GT/RT 重叠和进程 CPU 时间；新旧 PrepareFrame 包含的等待范围不同。

`Stress/Objects` 表示配置对象数。upload 路径的 `Stress/ObjectUploadBytes` 表示当次对象准备的实际 host 写入字节。
draw 使用 Renderer 帧编译，观察 `SceneDraw::CullAndSelect`、`BuildBatches`、`Draw` 与 `SceneGpuData::Prepare`；旧 legacy-draw 已删除。
多个视图共享对象 buffer，物理 flight
则分别追赶自上次使用以来的变化，因此每帧上传对象数不必等于本次 GT changes。静止负载在各 flight 初始化后应为零上传。
GT 的 `Stress/LastCompletedFrameLatencyMs` 和 `Stress/LastResolvedGpuMs` 分别记录最近完成帧的延迟及最近 resolve 的
GPU 时间（毫秒），不是当前 GT 帧的即时耗时；关闭 GPU profiler 时不输出后者。
在对象规模不变时对比不同变化量，再固定变化量对比规模；双线程的等待和吞吐会相互影响，不能直接用两次总帧时间相减
作为精确的某层 overhead。优先比较相应 CPU zone 的执行时间，并用单线程捕获辅助确认。
此样例也不代表真实资产流式加载、多材质或完整游戏帧率。

## World → RenderScene 状态同步基准

`bench_scene_sync` 使用真实 Actor、StaticMeshComponent、Directional/Point/SpotLightComponent、
World 与 SceneManager。每个 Shape 对应一个 Actor 和一个 Mesh 组件，Mesh 资产已 Ready；未启用 Tick 的
对象不进入 World ticking 列表，空闲帧只付列表遍历（默认为空）。该基准截止于 CPU `ConsumeRenderUpdates/Apply`，不含可见性、draw 准备、命令录制、
GPU、资产 IO 或真实游戏逻辑。没有 RectLightComponent，故不模拟 Rect 组件。

同一负载均运行单线程和 GT/RT 双线程，flight 数分别为 1/2/3。双线程按顺序消费有界 flight，GT 只在
复用占用的槽位时等待，F=2/3 允许 GT/RT 重叠。CPU 测试在 Consume 和读者结束后回传
`OutputsExecuted=false` 的完成通知；不把这个通知当作真实 GPU fence。

| 负载 | 参数 |
|---|---|
| 独立 Shape 变换 | 1k/10k/100k；每帧 dirty 为 0、1、1%、10%、100% |
| 顺序访问对照 | 10k/100k 全量变换，按创建顺序访问；其余变换场景使用随机排列 |
| 重复 setter | 10k Shape 的 1%；分别写 1/10/100 个不同值；另测 100 次同值短路 |
| 层级 | 10k Shape；链长 1/4/16，改叶子或根；每组 4/16 节点的宽树改根 |
| Ready mesh 改绑 | 10k Shape 的 1%，在两个 Ready 资产之间切换 |
| 分类光源 | 1/16/64/256；无 dirty、每帧改单灯、改全部；Directional/Point/Spot 轮流创建 |
| 生命周期 | 10k 常驻，每帧等量创建/删除 10；另测每 32 帧替换 10% |
| Reparent | 10k Shape 的 1%，在两个无几何父节点间 KeepLocal 重挂接 |
| 混合 | 10k Shape、64 灯；1% 变换、4 灯变化、10 创建/删除；每 32 帧全量变换 |
| 真实巡游 | 100k Shape、64 灯；0.1% 变换、1 灯变化 |
| 真实交火 | 100k Shape、64 灯；1% 变换、8 灯变化、0.05% 改绑、每 16 帧替换 0.2% |
| 开放世界流式 | 20k Shape、32 灯；1% 变换、2 灯变化、每 8 帧替换 2.5% |
| 群体动画 | 2k Shape 组成 1 根 + 2 挂件的宽树；每帧全部根移动、16 灯 2 变化 |
| 过场光照 | 10k Shape、256 灯；2% 变换、全部灯每帧变化 |
| 编辑器空闲 | 20k Shape、8 灯；单个物体移动、无灯变化 |

后六项是真实用例负载：按典型帧的比例组合上面的单轴机制，不引入新机制。同一帧内的移动、改绑与
替换取互不重叠的选择偏移，因此更新包条数可精确断言；带层级的负载不参与替换，避免销毁带跨 Actor
子节点的父 Actor。这些负载从测试循环直接写组件，用来测吞吐。由 Actor 与 Component Tick 推动的
巡游、交火、流式换块、群体、暂停、过场灯光和编辑器拖拽在 `FrameScenarios` 里逐帧核对场景内容。

随机选择序列使用固定种子，在 fixture 构造时生成；单/双线程使用相同序列。
`modules/runtime/tests/scene_sync_workload.cpp` 提供无计时器的共享负载与有界 flight 驱动。
`test_scene_sync.cpp` 只注册 GTest：Shape 缩到 256，40 帧逐帧核对身份、mesh、矩阵、bounds、
光源参数和删除结果，覆盖 burst；消费闸门验证 GT 可以先发布全部 F 个 flight。
另有小规模共享/独立 mesh、不同层级深度和多视图的生命周期帧验证。性能入口在
`benchmarks/bench_scene_sync/bench_scene_sync.cpp`，不链接 GTest、不执行这些正确性校验。

Windows CPU 专用构建示例（同时提供 Debug 正确性目标）：

```powershell
cmake -S . -B build_scene_sync_perf -G "Visual Studio 18 2026" -T ClangCL -A x64 `
  -DCMAKE_MSVC_RUNTIME_LIBRARY='MultiThreaded$<$<CONFIG:Debug>:DebugDLL>' -DMI_STATS=OFF -DMI_PROFILE=OFF `
  -DRADRAY_ENABLE_PROFILER=OFF -DRADRAY_BUILD_SHADER_COMPILER=OFF -DRADRAY_BUILD_EXAMPLES=OFF `
  -DRADRAY_ENABLE_D3D12=OFF -DRADRAY_ENABLE_VULKAN=OFF `
  -DRADRAY_BUILD_BENCHMARKS=ON -DRADRAY_ENABLE_ZLIB=OFF -DRADRAY_ENABLE_LIBJPEG=OFF
cmake --build build_scene_sync_perf --config Debug --target test_radray_runtime --parallel 4
ctest --test-dir build_scene_sync_perf -C Debug -R '^SceneSyncCorrectness\.' --output-on-failure
cmake --build build_scene_sync_perf --config Release --target test_radray_runtime bench_scene_sync --parallel 4
build_scene_sync_perf/_build/Release/bench_scene_sync.exe --benchmark_list_tests=true
python tools/run_scene_sync_benchmark.py --build-dir build_scene_sync_perf --output build_scene_sync_perf/results/native `
  --benchmark_filter='SceneSync/(shape_10000_dirty_100|chain_16_root)/' `
  --benchmark_min_time=0.5s --benchmark_min_warmup_time=0.2 --benchmark_repetitions=5
```

SceneSync 与 Lifecycle/Frame 使用 Google Benchmark 的自动迭代、预热、重复采样和标准 reporter；
不使用逐帧时钟和自制样本统计。命令 storage 等专项可以使用 `UseManualTime` 分离 Reset 与 Record，不能混同这些计时边界。
预热默认为 Google Benchmark 的默认值（不预热），正式比较显式指定上述 warmup/min-time/repetitions。
均值、中位数、标准差和 CV 是独立 repetition 结果的统计，不是逐帧 P95/P99。

SceneSync 与 Lifecycle/Frame 用 `KeepRunningBatch(256)` 测完整批次。每批连续处理 256 帧，
允许 F=2/3 的 GT/RT 重叠，批末在计时内完成所有 Apply、CPU completion 和资产 Pump。
Google Benchmark 的 `iterations` 与 `Time` 分别按帧数和每帧墙钟时间报告，`items_per_second` 是帧吞吐量；
`CPU` 通过 `MeasureProcessCPUTime` 合计进程内 GT/RT 的 CPU 时间。这里不报告单帧端到端延迟。
每批一次排空属于测量边界，会限制跨批次重叠；它不是 GPU fence 或真实应用的帧率限制。
fixture 构造、初始场景同步、随机排列生成和销毁在计时循环外。Google Benchmark 自行决定执行多少批，
小的固定迭代请求可能被向上补齐到完整批次。帧索引按 65536 循环，保持周期负载与浮点 setter 的变化有效。

脚本只运行已构建目标，先独立启动 GTest，再调用 benchmark；`--benchmark_*` 参数按原样转交，
使用 `--benchmark_option=value` 形式。输出目录必须为空，保存原生 `results.json`、日志、命令、Git 补丁、
未跟踪负载源码、源码/二进制 hash 与 CMake 配置，不重新解释 reporter 里的时间。
旧 `--frames/--runs/--cases/--allocations` 及 `RADRAY_SCENE_SYNC_*` 采样变量已移除。
分配统计不混入延迟测量；当前 benchmark 不再提供自制 mimalloc 分配轮。
正式性能结论使用 Release，并固定编译器、allocator、profiler、机器负载与电源状态。
旧手工采样结果不能与新基准的数值直接比较。
World 紧凑变换存储的同机交替对照见[2026-09-24 实验报告](../history/2026-09-24-world-transform-store-benchmark.md)，
其中保留基线、最终二进制 hash、每轮结果与回退项；报告中的 CPU 同步吞吐不能当作实际 GPU 帧率。

## CPU 生命周期与 OBJ

性能测量独立放在 `benchmarks/bench_lifecycle/bench_lifecycle.cpp`，不从 test 源码通过宏切换入口。

| Benchmark | 计时范围 |
|---|---|
| `Lifecycle/CreateActors` | 创建整组 Actor/Component、共享或独立 Ready mesh 的构建与绑定；不含初始场景同步和销毁 |
| `Lifecycle/DeleteAndSync` | 提交本组删除、S1、Collect、Seal、RT Apply、CPU completion 和资产 Pump |
| `Lifecycle/Frame` | setter、Tick、S1、Collect、Seal、交付、Apply、多视图 CPU bounds 读取和 CPU retirement |

规模为 10k/100k，层级深度为 1/8，资产为共享/独立；创建和删除各自注册，删除量为 1/1%/50%。
Frame 再覆盖 F=1/2/3、单/双线程、变化量 0/1/1%/100%、1/3 个 CPU 视图，共 384 个独立帧负载。
创建/删除每轮用 Google Benchmark 的 `PauseTiming/ResumeTiming` 排除 fixture 准备和清理；
大批量创建/删除按一次整组操作报告 Time，并通过 `SetItemsProcessed` 报告 Actor 吞吐量。
Frame 的批次与计时规则同 World → RenderScene 基准，不包含可见性算法、真实 draw 或 GPU 执行，不能称为渲染 FPS。

```powershell
cmake --build build_scene_sync_perf --config Release --target bench_lifecycle test_radray_runtime --parallel 4
ctest --test-dir build_scene_sync_perf -C Release -R '^LifecycleScale\.' --output-on-failure
build_scene_sync_perf/_build/Release/bench_lifecycle.exe --benchmark_filter='Lifecycle/(CreateActors|DeleteAndSync)/count:10000/depth:1/unique:false' --benchmark_min_time=0.2s --benchmark_repetitions=5
```

`bench_read_obj` 在 fixture 准备时从仓库根读取 `assets/buddha1.obj`，每轮测内存文本 reader 的构建、解析和销毁；
输入不使用全局缓存，文件缺失、空输入或解析失败报告 `SkipWithError` 并返回失败，不对空数据产生正常成绩。
该目标同样支持原生过滤、预热、repetitions 和 JSON/CSV reporter，不测磁盘 IO。

## Scene GPU 上传

采样 `scene_sync` 时，`SceneDraw::Draw` 是每视图的 CPU 绘制录制区间，
`SceneGpuData::Prepare` 下的 `ResourceUploader::TryUploadBufferRanges` 分为
`ResourceUploader::StageBufferRanges`（staging 预留、拷贝与 barrier 收集）和
`ResourceUploader::RecordBufferCopies`（逐范围 copy 命令录制）。GPU 时间使用后端帧时间戳。`SceneSync/LastResolvedGpuMs` 是最近一次 flight 完成后
解析的 GPU 帧时间，双线程下可能重复读取同一个值，不能与当前 CPU 帧逐项配对。
`SceneGpu/UploadObjects`、`SceneGpu/UploadRanges` 报告本次物理 flight buffer 的上传量；
同一 Scene 同帧多视图复用已准备的 buffer，不重复发出这两项采样。
默认每帧移动父节点，约三分之一对象跟随；对象槽位每隔三个分布，可能产生大量小上传范围。
`--instances` 控制对象数量；当前通过 VisibleSlots 自动实例化，按可见性、section 和 winding 分批。

`bench_scene_gpu` 是独立 Google Benchmark 目标，不依赖 JIT。固定 10k mesh 参数条目，变化量为 0/1/100/10000，
稀疏修改按均匀步长分布到槽位，100 个变化对应 100 个不相邻范围；全量变化合并为一个范围。
另含父节点驱动 10k 子对象、每帧增删 10 对象，以及 `once` 单次修改后静止；F=1/2/3/4，1/3 次视图准备。`PrepareAndRecord` 只计对象参数准备及上传命令录制，排除 GT 修改、Apply、提交与等待；
`SubmitFrame` 计 GT 修改、Apply、准备、真实主队列提交及 flight 重用等待。该基准不录制 geometry draw，
不能解释为完整渲染帧率。设备 profiler 的 `gpu_ms/frame` 是提交的 GPU 命令时间，上传字节/范围是每帧统计差值。
`CpuSync` 单独计修改、Collect、Seal 和 Apply，通过原生 PauseTiming 排除 GPU flight 等待与 BeginFrameRecord；打包与上传命令录制仍按组合成本报告。
初始化和预热不混入稳态结果；`first_use` 单独测首次准备 10k 条目，`growth` 先把各物理 buffer 初始化为 8192 条目，再测扩到 10k 条目的重新初始化。
冷启动两类仅计 `PrepareAndRecord`，场景创建与初始同步不计入该时间。双线程 CPU 同步继续由 `bench_scene_sync`/`bench_lifecycle` 测量。

```powershell
cmake --build build_release --config Release --target bench_scene_gpu bench_scene_sync --parallel 12
build_release/_build/Release/bench_scene_gpu.exe --benchmark_filter='SceneGpu/.*/.*/F2/' --benchmark_min_time=0.2s --benchmark_min_warmup_time=0.1 --benchmark_repetitions=3 --benchmark_out=build_release/scene_gpu.json
```

必须先完成构建和正确性测试，再独立采样。CPU 基线与新增 GPU 成本分别报告，不把历史约 500 μs 当作跨配置绝对门槛。

## 命令存储与绑定

`bench_command_allocator` 的 `CommandAllocator/` family 用相同 copy 录制比较共享 RHI storage 与每 command 独立 storage，
覆盖 M=1/8/32/128、K=1/2/4。初始化与第一次 native 扩容在计时外，输出 native capacity、Reset 与 Record 微秒数。
该合成基准不 Submit，因而没有 GPU 时间、运行中的存储占用或帧等待成本，不能当作帧率。
真实窗口/离屏负载使用 `framework_stress --mode=draw`；重构前比较使用冻结的旧程序包，当前不保留旧执行分支。
两条路径都使用当前 RHI allocator；这组 A/B 衡量公共 pipeline 的接入成本，不代表旧提交的 allocator 性能。
用 `--no-gpu-profiler` 对照运行时 GPU profiler 开关；`--views=N` 不再限制为 1 或 3。
native storage 字节数由驱动持有，无法直接测得；进程 working set 不能直接等同命令存储占用。
2026-10-07 的配置、原始数据、结果和当时未覆盖项见[第一阶段验收记录](../history/2026-10-07-phase1-rhi-command-allocator-validation.md)。

性能用 Google Benchmark 串行采样，关闭原生验证并显式选择 RHI 开关。
`CommandAllocator/` 和 `D3D12Descriptor/` 固定关闭 RHI 校验；前者输出原生 allocator、command、
graphics encoder、parameter set、graphics PSO 的 `sizeof_*`，仅表示 C++ 对象字节数，不含堆内存或驱动分配。
`CommandAllocator/<backend>/shared/M<n>/K<k>` 的共享策略由 benchmark 显式管理：D3D12 创建 K 个 RHI allocator，
第 i 条 command 固定绑定第 `i % K` 个；Vulkan 只创建一个 pool。`per_command` 每条命令各用一个 allocator。
两端 `native_capacity` 与 `rhi_allocators` 均按实际 RHI allocator 数报告；RHI 本身不自动创建额外 allocator。
`RhiBinding/<backend>/raw|validation` 每次录制 128 次真实参数集绑定，初始化和首次扩容在计时外，
不提交 GPU；`items_per_second` 是绑定吞吐，Time 是整次录制的 CPU 墙钟耗时。
该基准读取仓库内 compute artifact，无需运行 shader compiler。

```powershell
cmake --build build_release --config Release --target bench_command_allocator bench_d3d12_descriptor --parallel 4
build_release/_build/Release/bench_command_allocator.exe --benchmark_filter='^(CommandAllocator/(d3d12|vulkan)/shared/M32/K1/manual_time|RhiBinding/.*)$' --benchmark_min_time=0.2s --benchmark_repetitions=3 --benchmark_out=build_release/rhi-record-bind.json --benchmark_out_format=json
build_release/_build/Release/bench_d3d12_descriptor.exe --benchmark_filter='^D3D12Descriptor/Publish/(0|1)$' --benchmark_min_time=0.2s --benchmark_repetitions=3 --benchmark_out=build_release/rhi-descriptor.json --benchmark_out_format=json
```

比较前后结果须固定编译器、配置、profiler、机器负载与设备。Debug 前后采样仅作回归线索，
正式性能结论使用 Release；校验关闭时由工厂直接返回后端，公共 GPU 用例同时核对该对象身份。

## Descriptor 发布与分配

descriptor 发布微基准在 Google Benchmark 的 `bench_d3d12_descriptor` 中，不计入普通测试。先构建 Release 目标，然后显式运行：

```powershell
cmake --build build_release --config Release --target bench_d3d12_descriptor --parallel 4
build_release/_build/Release/bench_d3d12_descriptor.exe --benchmark_repetitions=5
```

输出 scenario 0–5 分别表示单 CBV、连续 64 元素、64 元素中每隔 7 项更新、重复相同 Set、
Vertex/Pixel 各 32 元素、显式 Vertex/Pixel 两个目标各 64 元素。
`D3D12Descriptor/Publish` 是每轮 Set 与 Flush 总耗时；`D3D12Descriptor/Allocation` 是创建并销毁
一个 set 的耗时。性能基准关闭 D3D12 debug layer 与 GPU-based validation；正确性与验证层诊断由对应 GTest 负责。基准不提交 GPU 命令，不代表 draw/dispatch 或 GPU 执行时间。

## Raster 准备

`bench_raster_prepare` 分别测纯 CPU graph 构造/编译、double bounds 与通用 primitive/group 排序；它不创建 command 或提交 GPU。使用 Release target、`--benchmark_min_time=0.05s --benchmark_repetitions=3 --benchmark_out=results.json --benchmark_out_format=json` 保存结果。Scene 的同步、PrepareAndRecord 与 SubmitFrame 分解沿用 `bench_scene_gpu`；`changed:0/1` 与多个 view 可分别观察 warm 零上传和单对象更新。

## 真实材质和实例化压测

开启 RADRAY_BUILD_BENCHMARKS、RADRAY_ENABLE_SHADER_JIT 与 RADRAY_ENABLE_PROFILER，构建后串行运行：

```powershell
cmake --build build_release --config Release --target bench_material_scene --parallel
python tools/run_material_scene_benchmark.py --exe build_release/_build/Release/bench_material_scene.exe --output build_release/perf/final --no-traces
```

上例只运行 benchmark。采集 trace 时移除 `--no-traces`，通过 `--tracy` 指定本机匹配版本的 Tracy 工具目录（含 capture、csvexport）。
脚本默认三次重复、0.1s 最小计时，可用 `--repetitions` / `--min-time` 调整，保存 Google Benchmark JSON、Tracy 原始 trace 和 CPU zones CSV。
默认矩阵覆盖两个后端 N=1k/10k/100k、M=1/100、V=1/3、dirty=0/1%，--filter 可以补独立材质、密集 dirty、Direct/Manual/ManualTint 与 churn。
变换与参数压力固定更新所选实例，并交替写入不同值，保持长采样中的实际脏比例；Manual/ManualTint 使用一个共享材质的显式集合。
每个 fixture 先预热八帧；PrepareRecord 排除 GT/Apply/等待/Submit，包含 SceneGpu、SceneDraw 准备和真实 record；
FullFrame 包含各阶段及 flight 等待，256×256 离屏 GPU 时间另用 timestamp 统计。GPU/CPU 时间不能相加为 FPS。
`--filter` 还可选阶段隔离用例：`MaterialScene/CompileOnly/...` 只在 `FrameBuilder::Compile` 内恢复计时，`MaterialScene/CompletionCpu/...` 在 `CompleteFlightIfReady` 返回后才恢复计时；
两者用 PauseTiming/ResumeTiming 排除 fence 等待、Submit 与录制，draw 仍真实录制并提交。CompileOnly 固定 N1000/V3/F3、tint 取 `i%7` 扫描 M=1/7/32/100/300，所以 M>=7 时最终 Material 值仍只有 7 种；
注意该用例的 M 同时抬高 draw 数，Compile 随 M 超线性增长，成因尚未定位（见修复计划"待验证"）；CompletionCpu 取 M=1/100。
`MaterialScene/OverrideChurn/...` 用公共 `Renderer` API 直接构造 M1/V1，每个 draw 带一份不同的 `MaterialConstants` override，使帧内唯一组数等于 draw 数（N=100/1000/4000）：这是帧级备忘若退回线性扫描会呈 O(draws²) 的形态，用于防止该回退。
`Renderer::Compile`、`CompileDraws`、`ProcessCompletions`、`Collect` 带 `RADRAY_PROFILE_SCOPE_N`，Tracy 中可把 Collect 与完成清理的容器析构分开。2026-10-16 的初始对照见[基线 CSV](../history/2026-10-16-render-framework-regression-fix-baseline.csv)，2026-10-17 的修复后数值与 A/B 见[修复计划](../history/2026-10-16-render-framework-regression-fix-plan.md)末尾。

```powershell
build_release/_build/Release/bench_material_scene.exe --benchmark_filter='MaterialScene/CompileOnly/' --benchmark_min_time=500x --benchmark_repetitions=3 --benchmark_out_format=json --benchmark_out=build_release/perf/compile-only.json
build_release/_build/Release/bench_material_scene.exe --benchmark_filter='MaterialScene/CompletionCpu/' --benchmark_min_time=1000x --benchmark_repetitions=3 --benchmark_out_format=json --benchmark_out=build_release/perf/completion-cpu.json
build_release/_build/Release/bench_material_scene.exe --benchmark_filter='MaterialScene/OverrideChurn/' --benchmark_min_time=200x --benchmark_repetitions=3 --benchmark_out_format=json --benchmark_out=build_release/perf/override-churn.json
```
Tracy trace 包含初始化，统计时使用 warmup complete message 排除预热事件；不使用整段 capture 平均值替代稳态。默认采两后端，专项 trace 可用 `--backend d3d12` 或 `--backend vulkan` 限定后端。
三种 churn 分别发生在 capture 前、capture 后未 Seal、Seal 后；UniqueAssetChurn 每轮新建 mesh/material，不能与静态独立材质分布混同。
独立材质压力受后端 descriptor 容量约束；D3D12 默认 shader-visible resource heap 为 65,536 个，
benchmark 初始化前检查所需容量，超预算样例报告不支持，不输出成功计时。
用 --memory-case 保存独立进程 working set/private bytes 曲线；它是进程内存观测，不等于原生显存或 descriptor pool 字节。
程序、测试与其他 GPU 负载串行运行。实例用法和 2026-10-13 历史验收见[实例绘制](instancing.md)与[性能报告](../history/2026-10-13-material-instancing-performance.md)。

## GPU 生命周期对照

MaterialScene 的 F2 基准名称保持兼容；F1/F3/F4 在同名末尾追加 /F1、/F3、/F4。场景矩阵包含 dirty=0/1/10/100、N=1k/10k/100k、V=1/3，以及 Direct/Manual/ManualTint 和 churn。CPU 默认值随 Material 描述保存；GPU 完整参数组按兼容布局与最终值共享。性能运行与构建、测试和其他 GPU 负载串行进行。

`bench_command_allocator` 还注册 `RenderScope/`（稳定、clear、load/store、尺寸版本、重建、预算）、`FlightSubmission/`（单批、多批、后续 gate）与 `WindowSubmission/`（F1–F4、1/3 个真实窗口、稳定、连续 resize、输出跳过）。窗口基准保留单独必要 copy，并在 drain 后回读核对；其输出跳过由真实 acquire 后的 presentability 谓词控制。RenderScope 只测 CPU 原生准备/编码，未提交；后两类分别报告 submit CPU、GPU 与完成延迟，gate 时间包含显式依赖间隔。窗口性能测量关闭 validation；[多窗口正确性回归](build-test.md#vulkan-多交换链同步验证已知问题)使用修复后的 validation/sync validation，结果分别报告。

```powershell
python tools/run_gpu_lifecycle_benchmark.py --baseline-bin build_release/gpu-lifecycle-baseline-build/_build/Release --output build_release/gpu-lifecycle-perf --repetitions 3
python tools/run_gpu_lifecycle_benchmark.py --mode coverage --output build_release/gpu-lifecycle-coverage --repetitions 1
python tools/run_material_scene_benchmark.py --exe build_release/_build/Release/bench_material_scene.exe --output build_release/gpu-lifecycle-traces/final --traces-only --tracy C:/Tools/Tracy
```

`--baseline-bin` 目录需预先放置匹配版本的 `<target>.baseline.exe`；当前版本由 `--bin` 指定，默认 `build_release/_build/Release`。
`C:/Tools/Tracy` 是示例路径，换成实际目录。comparison 默认交错启动三对独立进程；coverage 的 1x 只证明矩阵可运行，
SceneSync/Lifecycle Frame 的 `KeepRunningBatch(256)` 会把小迭代请求补齐为整批。SceneGpu 冷启动／扩容和 Lifecycle 删除固定采样五次，场景重建在计时区间外；针对性删除复验可用固定 50x 与更多独立配对。不要用自适应 min-time 在这些场景上反复重建来估计稳态。原始 log、JSON、命令、二进制 SHA256 与配对区间留在输出目录，Tracy 以最后一条 warmup message 截断冷区间。默认 trace filter 用首尾锚点限定 F2，避免与追加 /F1、/F3、/F4 的用例混采；其他 F 对照也须使用同一 flight 数与完整名称。静态材质的分配和 Set/FlushWrites 零增量由已有 fixture probe 正确性测试验证。

## Descriptor 冗余离屏验收

构建 example_descriptor_reuse 后，串行运行独立进程并冻结程序、shader、artifact、源码 ZIP 和 SHA-256：

```powershell
cmake --build build_debug --config Debug --target example_descriptor_reuse --parallel 4
python tools/run_descriptor_reuse.py --exe build_debug/_build/Debug/example_descriptor_reuse.exe --output build_descriptor_reuse/measurement
```

输出目录必须不存在。默认双后端各 21 个 case；快速排障加 `--quick`，不能代替完整验收。
每 case 预热 2×F 帧，再运行 30 帧稳态；默认 128 对象、32 材质、3 视图、3 flight，目标 256×256。
覆盖等价独立 pass、兼容不同 PSO、Color/Depth、同值/变化 override、不同可见顺序、规模扫描及视图数升降。
probe 由样例持有，记录真实参数集 Create/Set/Flush、实际 D3D12 table 槽位（含空洞）、Vulkan set/类型数量，
区分完整组原生绑定等价重复与同一语义数据的多个切片。Vk 数字不是驱动内部字节数。
检查 GPU 读回、负载回落第 1/F/61×F 帧、drain 及注销后占用；缺后端或跳过不算通过。
混合组迁移必须单列 ABI 配对，不与 shader 不变的 case 混算。保持 heap 容量一致。
这个程序验证分配和正确性，不推导 FPS；耗时仍由 Google Benchmark 的 MaterialScene/SceneGpu 等基准测量。
