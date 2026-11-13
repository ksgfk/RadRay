> - 适用: 选择当前基准、比较改动或解读历史性能报告
> - 权威: 基础模块当前测量入口；历史数值只属于各报告基线
> - 锚点: `benchmarks/CMakeLists.txt`, `benchmarks/bench_read_obj/`, `benchmarks/bench_sparse_set/`, `benchmarks/bench_rdg_upload/`, `tools/measure_rdg_upload_gpu.py`, `modules/runtime/src/frame_scheduler.cpp`, `modules/runtime/src/frame_slot.cpp`, `modules/runtime/src/render_graph_pipeline.cpp`, `modules/runtime/src/scene.cpp`, `modules/runtime/src/mesh_draw_command.cpp`, `modules/runtime/src/rdg.cpp`

# 性能测量

先按[构建与测试](build-test.md)完成正确性回归。性能目标通过 `RADRAY_BUILD_BENCHMARKS` 开启，使用 Release。
构建、测试、benchmark 与 trace 串行执行。

## 当前入口

| 入口 | 测量边界 |
|---|---|
| `bench_read_obj` | 内存 OBJ 文本解析，不含磁盘 IO |
| `bench_sparse_set` | SparseSet 的扩容、访问、删除、复用与历史高水位压力；无需外部资产 |
| `bench_rdg_upload` | runtime 的声明、编译、准备、录制及上传页复用；fake 只记录 GPU Copy 命令 |
| `bench_rdg_upload_gpu` | 真实 D3D12/Vulkan 上传与中间消费者；独立外围 timestamp 及 CPU Prepare/Record |

## RDG 上传

`bench_rdg_upload` 随 runtime 与 benchmarks 开关启用，无需 shader 或真实 GPU。
`RDGUpload/<负载>/<阶段>` 分别测 Build、Compile、Prepare、Record、Frame（Build 至状态发布）和 Recycle。
每个阶段仅对目标区间计时，fixture 以及非目标阶段暂停计时；Frame 不包含槽等待或回收。
Recycle 单独测下一次 BeginFrame 的 fake 等待、命令重置、帧对象析构和线性存储重置，不代表真实 GPU 等待成本。
Prepare 包含布局、物化、分配、填充与 Flush，真实参数组更新成本应结合样例的 PrepareBindings Tracy 区间观察。
`RDGUploadComponent/PrepareLayout|AllocateFill|Flush` 独立测 64 个 80 字节来源的组件调用；
这些微基准不包含图调度和真实驱动绑定，不能相加代替完整 Prepare。

负载覆盖 1 MiB Borrow/Snapshot、固定总量 64/1024/4096 块、0/10/100% 存活、固定生成、
按可见命令数量决定大小的动态切片（声明上界为 16 个元素，实际 1/8 个）、不重叠与重叠写入、中间消费者及首次分配。
Snapshot 单独列出；Fresh 每轮在计时外释放池/页，首次创建计入被测准备阶段；其他负载先预热并保留页。
图对象与跨阶段容器使用FrameSlot 的线性 PMR，阶段局部工作容器使用独立 scratch pool，与 Pipeline 配置一致。
每帧先析构帧对象，再重置线性存储；临时容器在作用域结束时归还 pool 单元。
Fresh 只重置 GPU 池/页，CPU arena 与 pool 仍按正常槽规则复用，不代表整个进程的冷分配。
常规负载的来源创建不生成完整中间 payload；性能 fake 的 Copy 只记录次数和字节数，不用 memcpy 模拟 GPU 执行。

```powershell
cmake --preset win-x64-release-clangcl -DRADRAY_BUILD_RUNTIME=ON -DRADRAY_BUILD_BENCHMARKS=ON -DRADRAY_ENABLE_PROFILER=OFF
cmake --build --preset win-x64-release-clangcl --target bench_rdg_upload --parallel 8
. tools/dev_env.ps1 -Triplet windows-x64-clangcl -Config Release
$bench = './build/windows-x64-clangcl/Release/_build/Release/bench_rdg_upload.exe'
& $bench --benchmark_min_time=1x
& $bench '--benchmark_filter=RDGUpload/(LargeBorrow|Chunks64|Chunks1024)/(Build|Compile|Frame)' --benchmark_min_time=0.15s --benchmark_repetitions=5 --benchmark_out=build/rdg-upload.json --benchmark_out_format=json
```

基准固定 FullValidation=true；fake 不启用 RHI validation，两者与真实后端测量分别说明。
`logical_uploads` 是声明数，`physical_copies` / `copy_bytes` 是录制数与有效字节，
`barrier_descriptors` 是 RHI 屏障描述数；这些不能代替原生 GPU 时间。
`resource_creates`、`maps` 统计每轮资源创建/映射，`retained_upload_capacity` 是缓存页容量。
`--rdg_memory_probe` 只注册 Frame 用例，通过 FrameScheduler 的自定义上游入口观测槽内全部 CPU PMR 存储，
上游继续使用 HeapMemoryResource；普通计时模式使用默认上游，不经过 probe。
采样模式的时间包含 probe 开销，不能与普通模式时间比较。
`peak_storage_bytes` / `retained_storage_bytes` 为上游峰值 / 最终槽回收后的保留量，包含堆对齐取整、
线性 arena 的初始缓冲和额外块、scratch pool 缓存；`upstream_allocations` 为每个被测帧的上游分配次数。
峰值包含预热首次分配，次数只计算被测帧；保留量不是对象和容器的有效数据量，也不单独区分 arena 与 pool。
这些指标不包含 arena/pool 外的来源 payload、Context、PassData、Pass 名称字符串、GPU 池、内联对象或系统分配器内部开销。
测量结束销毁 Scheduler 后，probe 检查全部槽的上游存储均已归还。

```powershell
& $bench --rdg_memory_probe --benchmark_min_time=32x --benchmark_out=build/rdg-memory.json --benchmark_out_format=json
```

比较性能使用普通模式，固定 CPU affinity（尤其混合核心 CPU）、构建/校验/profiler 配置，并交替运行改前改后版本。
复用时创建/映射为零不表示缓存内存已经释放；裁剪工作不包含外部 owner 已创建资源的成本。

## 比较与证据口径

`bench_rdg_upload_gpu` 每帧固定 512 个 4 B buffer 上传、512 个 1×1 RGBA8 texture 上传，以及一个中间 buffer
读回消费者；共 4096 B 上传、两个 Upload execution group。使用 Release、一个槽、Direct queue、默认高性能适配器，
关闭 driver/RHI validation，RDG FullValidation 保持开启。每帧提交并等待完成；这是小上传压力基准，不代表游戏的帧并行或 Present。
正确性与 validation 由 runtime 的 GPU 读回测试单独检查。

两个独立 RHI timestamp 包围完整 Record（含入口/出口屏障、拷贝和 Tracy scope 的结束），每帧只做一次外围 resolve。
计时包含外围 Tracy `RDG benchmark frame` scope 的开销，不使用子 zone 求和；三个 profiler 状态均使用同一外围计时。
Prepare/Record 用 CPU steady clock 测量，不含图声明、Compile、提交或等待。默认预热 200 帧，再测 1000 帧，输出各阶段
p50/p95/p99（排序后的向下取整分位点）；`--rdg_raw=<path>` 可保存逐帧 CSV。Google Benchmark 的 Time 列为 GPU 区间均值。

```powershell
& $gpuBench --rdg_backend=d3d12 --rdg_warmup=200 --rdg_samples=1000 --rdg_raw=build/upload-samples.csv
& $gpuBench --rdg_backend=vulkan --rdg_capture_wait
```

`--rdg_capture_wait` 在 GPU 初始化前等待 stdin 换行，供外部 capture 连接。
`tools/measure_rdg_upload_gpu.py` 接收 `--baseline-off`、`--baseline-on`、`--fixed-off`、`--fixed-on` 四个同负载
二进制，以及匹配 client 协议的 `--capture`、`--csvexport` 工具和 `--output` 目录。
它固定进程 affinity（默认 `0xff`，可通过 `--affinity` 修改），按后端和 profiler 关闭／未连接／已连接分组，
每组交替执行三轮独立进程。保存命令、二进制 SHA-256、逐帧 CSV、Tracy trace、导出的 GPU zone、各轮 JSON 与分位数中位值。
连接状态通过导出的外围 GPU zone 数检查；Tracy 最后一帧可能尚未回收，因此允许比执行帧数少一帧。
Tracy 0.14 的常规 GPU CSV 只含正时长 zone，行数是数量下界；需要精确数量时，`--timeline-export` 接受一个
读取原始 GPU timeline 的离线工具（参数为 trace 路径，输出 `name,complete,gpu_ns` CSV），包含零时长和未回收时间戳的 zone。
脚本分别保存三种计数，并核对完整 timeline 的上传、消费者与外围 zone 数。
该脚本不提供原生 API 拦截计数；由 zone 数换算的 timestamp/resolve 数必须标为按后端实现推导，不能当作原生抓帧实测。

基线记录提交、未提交补丁、负载源码、构建配置、完整命令及二进制 SHA-256。基线带工作树修改时明确说明，
不能用纯净提交代替。前后固定机器、编译器、allocator、profiler/validation、flight、视图、材质分布和脏比例；
交替运行独立进程，区分一次覆盖扫描与多轮配对比较。脚本选项不等于结果已经可比，仍需核对实际匹配的 case。

初始化、首次分配与稳态分开，使用 benchmark 的计时边界和预热配置区分。
当前基准不提供 runtime JIT、父节点传播或 GT/RT 并发测量；不能据它们推断渲染帧率或多线程收益。
比较修改负载时核对每轮实际执行的操作及数据变化，不把准备阶段算作被测操作。

| 指标 | 可以说明什么 | 不能代替什么 |
|---|---|---|
| CPU zone 墙钟时间 | 该阶段含驱动调用、调度在内的经过时间 | OS 纯 CPU 执行时间 |
| 进程 CPU 时间 | GT/RT 等线程的 CPU 时间合计 | 墙钟延迟或 GPU 时间 |
| repetition 中位数/CV | 多次测量结果及波动 | 单帧 P95/P99 |
| GPU timestamp | 所标记 GPU 区间的时长 | CPU 总成本或画面显示时间 |
| frame completion latency | 同一 frame 身份从开始到真实完成 | FPS 或 Present 返回时刻 |
| host/upload bytes | 本次提交的主机写入或上传量 | PCIe 总流量、显存驻留 |
| native allocation/block bytes | D3D12MA/VMA 的占用 allocation/预留 block | opaque RP/FB/descriptor 内存或 OS 驻留 |
| private bytes / working set | 进程内存曲线 | GPU 对象大小或显存 |

CPU 与 GPU 可以重叠，不把二者相加得到 FPS。短测的百分比、单轮覆盖以及亚微秒差异不能证明稳定收益；
分别检查 CPU、GPU 和尾部延迟，对疑似退化做独立长采样。预算拒绝、skip、error 与成功运行分列，
不把未运行 case 算作通过。drain 后缓存仍可能保留物理存储，回收正确性应检查使用引用和 owner 时点。

历史设计与实测数据见[历史目录](../history/README.md)。其中未入库的
`build*/` 日志、trace、源码归档仅是采样时的本机证据；文件缺失时需要重新测量，不能从报告重新生成原始样本。

## 性能采样（Tracy）

`RADRAY_ENABLE_PROFILER` 默认 ON，保留 Tracy client、CPU 插桩和后端 GPU 时间戳。
viewer 版本应与 project_manifest.json 的 Tracy tag 匹配；client 只监听 localhost、按需连接。
业务插桩只用 radray/profiler.h 宏，详见 [Core](../architecture/core-facilities.md#性能采样宏)。
CPU record/Submit 时间与 GPU 时间线分开解读。关闭使用 `-DRADRAY_ENABLE_PROFILER=OFF`。

### Runtime 分段

Runtime 通过相同开关输出以下 CPU zone。父区间包含子区间，不能把它们直接相加；
`RDG.Record` 和各 `Record*` 区间测量主机上的命令录制，不是 GPU 执行时长。
阶段之间的临时容器初始化/析构仍计入父区间，不保证子区间覆盖父区间的全部耗时。

| 区间前缀 | 计时边界 |
|---|---|
| `RadRay.FrameScheduler` | RunFrame、在途槽等待、WaitIdle |
| `RadRay.FrameSlot` | 命令存储重置、帧数据清理与登记对象析构、命令缓冲获取及首次创建 |
| `RadRay.FramePipeline` | Extract/Prepare/Record/Submit、应用帧数据提取、SetupGraph、逐 MeshPass 提取；Record 包含命令 Begin/End，Submit 包含提交钩子与状态发布 |
| `RadRay.Scene` | 持久缓存同步、可见命令收集与缓存直达指针解析（GatherVisibleHandles） |
| `RadRay.DrawList` | 可见命令稳定排序、可见列表和动态命令清理 |
| `RadRay.MeshDraw` | 整批命令提交；Debug 另有 Validate 子区间 |
| `RadRay.RDG.Compile` | 资源检查、访问检查与依赖构造、裁剪、调度与合并 |
| `RadRay.RDG` | Prepare、PrepareLayout、AcquireResources、FillUploads、PrepareBindings、Record/RecordGroup 和提交后的 CommitSubmittedStates |
| `RadRay.UploadArena` | 最终映射页 Allocate 和集中 Flush |
| `RadRay.RDG.Pool` | 每槽池复用标记重置、资源/视图查找；未命中时的 Create 子区间包含创建与池登记 |

RDG 在逻辑 Pass 录制区间使用 Pass 名称作为动态 CPU zone 名称。相同名称的 Pass 在统计中合并，
实例归属看时间线父区间。当帧视图创建包含在 Prepare，Record 不再创建视图。
采样粒度为阶段、Pass、资源获取或整批命令；收集和绘制循环内没有逐 Draw 事件。

Plots 按调用输出，多个槽或 Pass 共用同名曲线，通过所属 zone 的时间和
`FrameScheduler.Slot`、`FramePipeline.MeshPassId` 关联，不能把每个样本当作整帧总量：

- `Scene.VisibleBatches`、`DrawList.SortCommands`、`MeshDraw.SubmitCommands`
  分别表示本次 Gather 的输入 Batch 数、待排序命令数、提交列表长度；最后一项包含零实例而被跳过的项。
  命令数以 Batch/Pass 为单位；一条命令可按 ElementMask 发出多次 DrawIndexed，不能将列表长度当作 Draw 次数。
  GatherPrimitives 按 Batch 调用 Gather，不能把单个 VisibleBatches 样本当成该 Primitive 入口的合计。
- 当前没有 SnapshotCommands、SnapshotCapacityBefore/After 或 ClearSnapshots；缓存命令不再复制到帧槽。
- `Scene.DirtyBatches` 是缓存同步本次取出的脏 Batch 数，包括随后因失效而跳过的项。
- `RDG.DeclaredPasses/Textures/Buffers` 是声明数量，`AlivePasses`、`AliveMeshPasses` 和 `ExecutionGroups`
  是成功编译后的存活数量与合并组数。`Pool.Textures/Buffers/Views` 是槽开始复用时池中保留的条目数量。
- `RDG.UploadBytes` 是每个存活来源的实际有效大小，不含源行 padding 和绑定保留空间；Generated 同样计入。
  `GroupBarriers`、`FinalBarriers` 是对应批次的 RHI 描述符数，不含页起始同步及上传组内重叠写屏障；它们不是原生 GPU barrier 成本。

插桩关闭时宏及其参数表达式不参与运行。这些测点没有新增跨帧统计状态；开启时仍会增加采样成本，
性能比较需固定插桩版本、开关和负载，并将首次扩容/资源创建与稳态分开。

## OBJ 解析

`bench_read_obj` 在 fixture 准备时从仓库根读取 `assets/buddha1.obj`，每轮测内存文本 reader 的构建、解析和销毁；
输入不使用全局缓存，文件缺失、空输入或解析失败报告 `SkipWithError` 并返回失败，不对空数据产生正常成绩。
该目标同样支持原生过滤、预热、repetitions 和 JSON/CSV reporter，不测磁盘 IO。

## Sparse set

`bench_sparse_set` 使用 Google Benchmark，通过 `main` 内显式注册用例，不引入全局 fixture、分配钩子或生产观测字段。
容器契约见 [Core / SparseSet](../architecture/core-facilities.md#sparseset)。本基准针对当前实现的三数组布局、
swap-and-pop 删除、空闲链和 generation 检查设计；数据准备、校验和随机排列生成在计时外。
固定种子的 Fisher–Yates 排列可复现，顺序、随机与固定步长访问共享相同负载规模。

```powershell
cmake --preset win-x64-release-clangcl -DRADRAY_BUILD_BENCHMARKS=ON
cmake --build --preset win-x64-release-clangcl --target bench_sparse_set test_radray_core --parallel 8
ctest --preset win-x64-release-clangcl -R SparseSetTest --output-on-failure
. tools/dev_env.ps1 -Triplet windows-x64-clangcl -Config Release
$bench = './build/windows-x64-clangcl/Release/_build/Release/bench_sparse_set.exe'
& $bench --benchmark_list_tests=true
# 全矩阵只跑一轮，用于确认所有 fixture 与操作合法；不能用其时间做性能结论。
& $bench --benchmark_min_time=1x
# 分组长采样；换成 Insert、GrowthEdge、Read、DependentRead、Misses、Destroy 或 Churn 选择其它路径。
& $bench --benchmark_filter='SparseSet/Clear' --benchmark_min_time=0.2s --benchmark_repetitions=5 --benchmark_out=build/sparse-clear.json --benchmark_out_format=json
```

使用其它工具链时替换 preset 和输出路径。Debug 可做断言及 fixture 验证，性能比较使用相同配置的 Release。
完整矩阵包含较大工作集与昂贵的准备过程；平时按 family、payload 或参数过滤。原始 JSON 留在 `build/`，
不自动归入开发历史。失败调用 `SkipWithError`，同时使进程以非零退出；错误用例不能算作成功测量。

| Family / 对照 | 刻意触发的路径 |
|---|---|
| `Insert/Fresh`、`Reserved` | 新容器增长；Reserved 的一次 Reserve 在计时外，检查预留后仍存在的 dense 扩容 |
| `Insert/ReuseOrdered`、`ReuseRandom` | 容量充足时的空闲链复用；每轮按相同顺序或随机顺序删除，重新形成空闲链 |
| `Insert/ReserveEach` | 每次插入前调用 `Reserve(i + 1)`；故意模拟精确逐项预留对 sparse 数组增长的破坏，仅注册较小规模 |
| `GrowthEdge/Grow`、`Spare` | 通过 `Values().data()` 找到 dense 实际搬迁边界，比较临界点单次 Emplace 与已有余量的 Emplace；每批 8 个独立容器 |
| `Read/Values`、`Get`、`TryGet`、`IsAlive` | dense 遍历、句柄间接访问及存活检查；顺序、随机与 257 步长，跨不同缓存工作集 |
| `Read/*/Fragmented` | 按随机顺序逐项删除再插入，打乱 sparse→dense 映射，再用相同句柄顺序访问 |
| `Read/*/spread:64`、`Read/Get/Wide` | 固定 16,384 个活值分散在 1,048,576 个历史槽位；另设 4,194,304 个活值的随机读取以扩大缓存/TLB 压力 |
| `DependentRead` | 值保存下一个随机句柄，整环遍历；下一次地址依赖前一次加载，用于观察内存级并行难以掩盖的延迟 |
| `Misses` | invalid sentinel、范围外 Index、未复用死槽、已复用但 generation 失效；0/50/95/100% 命中及分组/打乱查询 |
| `Destroy/Tail`、`Forward`、`Random` | 连续删除末项与触发值搬移、反向索引更新、随机 sparse 写入的对照；每轮恢复初始布局 |
| `Churn/Tail`、`Random`、`Burst` | 固定存活数量，立即删除/重建与成批删除/成批插入；每轮改动约 1% 或 50%，循环覆盖整组句柄 |
| `Clear`、`ClearRefill` | 同一历史峰值下存活 0、1、1,024 或全部；反复空清空与清空后小量重建，暴露历史槽位扫描成本 |
| `ClearReservedEmpty` | 仅 Reserve、从未插入的空容器；与曾达到同等峰值后变空的容器区分，避免混淆 capacity 与历史槽位数量 |
| `ClearDistribution/Clustered`、`Random` | 同为一半槽位存活，比较成片与随机存活标记；每轮重新准备，保持分支分布 |

`P8/P64/P256/P4096` 分别为对应字节数的内联值，构造写满全部字段；读取只累计第一个 64-bit 字段，
大值读取因此也能显示「只用一个字段却有较大步长」的影响。`Owned64` 是独占一个 64 B 堆对象的 move-only 值，
用于分开观察内联值搬移与对象分配/析构成本；其 `payload_bytes` 是容器内对象的 `sizeof`，不包含堆块。

计时使用 `UseRealTime()` 的墙钟时间；`Time` 是**整批**时间，`batch_ops` 给出每批操作数，
`items_per_second` 是按此操作数归一化的速率。单次操作均值可用 `Time / batch_ops` 计算。
Churn 每对 Destroy/Emplace 算两次，Clear 算一次，ClearRefill 算一次 Clear 加 `live` 次 Emplace；
这些 family 的归一化口径不同，不能直接把所有 items/s 横向排行。`live` 表示操作前或稳态存活量，
`high_water` 表示曾创建的 sparse 槽数；GrowthEdge 两者按单个容器报告。

Insert 计入新值构造、句柄输出、隐式扩容及旧存储释放，但容器重置、一次性 Reserve 和复用准备在计时外；
ReserveEach 的逐次 Reserve 计入。`dense_relocations` / `relocated_elements` 来自独立的计时外地址变化探针，
不包含首次分配，也不等同于总分配次数或字节数，不能据此推断 sparse/反向索引的分配。
GrowthEdge 先预留 sparse，再测完整 Emplace；Grow 还可能伴随反向索引增长，Spare 则通过预插入再删除准备，
二者也分别走新槽与空闲槽路径，不能将差额全部归因于值 memcpy，更不能将批平均称为 P99。
Grow 每轮重建前缀；Spare 保留容量，仅在计时外删除上轮插入的末项，避免为一次无扩容插入反复重建整组大对象。

Destroy 的补充数据、Clear 的活值重建在计时外；ClearRefill 把重建一起计时。
Churn 计入驱动操作所必需的句柄表维护、循环索引及新值生成，校验期望值在循环后推导。
读取用累加结果、编译器屏障与公开逃逸的 fixture 地址防止整个遍历被折叠；修改路径使 dense 存储可观察。
计时外校验检查最终句柄、存活数和数据，不能取代 GTest 的完整正确性回归。

这里的 Fresh 指新容器，**不保证冷 CPU cache 或冷 allocator**。删除/重建准备会影响缓存温度，
固定查询序列会反复运行；需要跨缓存层解释时结合工作集和机器缓存配置，不能把一次随机读取笼统视作 DRAM 延迟。
每轮 fixture 恢复不计时，但会增加实际运行时长；单轮 smoke 和短采样仅确认覆盖与定位，稳定差异须按前文独立进程重复比较。
