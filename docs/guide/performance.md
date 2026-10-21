> - 适用: 选择当前基准、比较改动或解读历史性能报告
> - 权威: 基础模块当前测量入口；历史数值只属于各报告基线
> - 锚点: `benchmarks/CMakeLists.txt`, `benchmarks/bench_read_obj/`

# 性能测量

先按[构建与测试](build-test.md)完成正确性回归。性能目标通过 `RADRAY_BUILD_BENCHMARKS` 开启，使用 Release。
构建、测试、benchmark 与 trace 串行执行。

## 当前入口

| 入口 | 测量边界 |
|---|---|
| `bench_read_obj` | 内存 OBJ 文本解析，不含磁盘 IO |

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
| native allocation/block bytes | D3D12MA/VMA 的占用 allocation/预留 block | opaque RP/FB/descriptor 内存或 OS 驻留 |
| private bytes / working set | 进程内存曲线 | GPU 对象大小或显存 |

CPU 与 GPU 可以重叠，不把二者相加得到 FPS。短测的百分比、单轮覆盖以及亚微秒差异不能证明稳定收益；
分别检查 CPU、GPU 和尾部延迟，对疑似退化做独立长采样。预算拒绝、skip、error 与成功运行分列，
不把未运行 case 算作通过。drain 后缓存仍可能保留物理存储，回收正确性应检查使用引用和 owner 时点。

历史设计与实测数据见[架构地图的历史资料](../architecture/overview.md#历史资料)。其中未入库的
`build*/` 日志、trace、源码归档仅是采样时的本机证据；文件缺失时需要重新测量，不能从报告重新生成原始样本。

## 性能采样（Tracy）

`RADRAY_ENABLE_PROFILER` 默认 ON，保留 Tracy client、CPU 插桩和后端 GPU 时间戳。
viewer 版本应与 project_manifest.json 的 Tracy tag 匹配；client 只监听 localhost、按需连接。
业务插桩只用 radray/profiler.h 宏，详见 [Core](../architecture/core-facilities.md#性能采样宏)。
CPU record/Submit 时间与 GPU 时间线分开解读。关闭使用 `-DRADRAY_ENABLE_PROFILER=OFF`。

## OBJ 解析

`bench_read_obj` 在 fixture 准备时从仓库根读取 `assets/buddha1.obj`，每轮测内存文本 reader 的构建、解析和销毁；
输入不使用全局缓存，文件缺失、空输入或解析失败报告 `SkipWithError` 并返回失败，不对空数据产生正常成绩。
该目标同样支持原生过滤、预热、repetitions 和 JSON/CSV reporter，不测磁盘 IO。
