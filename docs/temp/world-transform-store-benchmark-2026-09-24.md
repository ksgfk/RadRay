> - 适用: World 紧凑 local TRS 存储原型的 CPU 场景同步对照与接口迁移评估
> - 权威: 2026-09-24 本机实验快照，不是跨机器性能承诺；当前接口以 render-framework 为准
> - 锚点: `modules/runtime/include/radray/runtime/game_framework/world_transform_store.h`, `modules/runtime/src/game_framework/world_transform_store.cpp`, `modules/runtime/src/game_framework/world_render_bridge.cpp`, `modules/runtime/src/render_scene/scene_writer.cpp`, `benchmarks/bench_scene_sync/bench_scene_sync.cpp`, `modules/runtime/tests/scene_sync_workload.cpp`

# World → Scene 变换存储原型测量

状态：实现和性能测量已完成。测量时为工作树原型，文档收尾时实现已纳入 `40f9f31bc180657f4e06c4a57b0795fa22b41514`；已核对当前源码与最终测量快照一致。基线为 `5c10ae4eedb3e9e94ffa1c25d3dbca415823ded7` 加开始本轮时已有的未提交改动；不是纯净 HEAD。日期：2026-09-24，最终候选为 `prototype_v3`。负数表示耗时降低。

## 结论

建议保留这一架构方向：把注册组件的 local TRS 集中到 World 的紧凑存储、按变化行/块批量发布，并把业务变换通知改为显式订阅，能显著降低大量对象同时移动时的框架成本，同时保留 GT/RT 分离、独立帧数据包、多 flight、增量更新及生命周期隔离。

F=2、GT+RT 双线程、五轮中位数：10 万对象全量随机更新从 **15.464 ms 降至 6.545 ms（−57.7%）**；全量顺序更新从 **8.509 ms 降至 2.359 ms（−72.3%）**。1 万对象全量随机更新降低 44.5%。这说明旧路径有大量可以消除的组织成本，传递所有变化数据本身并不能解释全部开销。

收益集中在密集更新与部分层级操作。普通关卡巡游、交火基本持平；稀疏更新没有普遍加速。反复写相同值、突发替换对象存在回退，详见下文。内存峰值测量按用户要求停止，不纳入结论。此版本是可运行并通过完整测试的架构原型，不代表所有工作负载都更快。

| 场景 / flight / 执行方式 | 原版 μs/帧 | 新版 μs/帧 | 耗时变化 | 原版 / 新版进程 CPU μs/帧 |
|---|---:|---:|---:|---:|
| `shape_10000_dirty_10000/F2/threaded` | 330.170 | 183.167 | -44.5% | 503.540 / 325.521 |
| `shape_100000_dirty_100000/F2/threaded` | 15464.152 | 6545.305 | -57.7% | 16601.562 / 8361.816 |
| `shape_100000_sequential_all/F2/threaded` | 8509.065 | 2359.009 | -72.3% | 10742.188 / 4333.496 |
| `shape_100000_dirty_1000/F2/threaded` | 46.631 | 48.566 | +4.2% | 81.380 / 82.265 |
| `chain_16_root/F2/threaded` | 32.762 | 24.418 | -25.5% | 55.650 / 32.552 |
| `reparent_subtrees/F2/threaded` | 41.512 | 28.341 | -31.7% | 75.120 / 40.690 |
| `level_walkthrough/F2/threaded` | 12.321 | 12.257 | -0.5% | 23.220 / 22.380 |
| `level_firefight/F2/threaded` | 115.118 | 116.263 | +1.0% | 164.795 / 169.542 |
| `open_world_streaming/F2/threaded` | 65.093 | 68.423 | +5.1% | 69.754 / 75.396 |
| `burst_replace_10pct/F2/threaded` | 17.605 | 19.373 | +10.0% | 16.735 / 19.215 |
| `same_value_100/F2/threaded` | 17.842 | 21.314 | +19.5% | 24.014 / 25.823 |

全量随机更新的进程 CPU 时间也由 16.602 ms 降到 8.362 ms；顺序更新由 10.742 ms 降到 4.333 ms。收益不只是改变线程重叠或等待位置。这里的进程 CPU 时间是各线程 CPU 时间合计，与墙钟耗时含义不同。

## 实现及保留的边界

下文是本次实验实现摘要，长期契约见 [Runtime 宿主](../architecture/render-framework.md#层级与渲染连接)。

1. 新增 WorldTransformStore，以紧凑的 48 字节 `{TransformId, LocalTransform}` 行保存注册组件的权威 local TRS；setter 直接写该存储。组件缓存内部指针，容量增长时统一修正。
2. dirty bit 去重，同时保留紧凑变化行索引和 64 槽位变化块。稀疏时一次 gather，密集时按块内连续范围复制；超过阈值才排序块索引。Collect 不再逐组件排序、解引用和重新打包 TRS，也不扫描整份静止场景。
3. parent/topology 与 local 数据更新分开；SceneWriter 用 epoch 懒失效重复更新定位信息，减少 Flush 后逐条回扫。
4. OnTransformChanged 采用显式订阅，子树维护订阅计数。没有业务订阅者且没有旧式渲染来源时，无需登记和遍历通知树。兼容旧式世界矩阵捕获的渲染组件继续走原有路径。
5. 增加 World::SetLocalTransforms 批量入口及带 generation 的 WorldTransformId。先验证整批句柄，再写值；非法批次不产生部分修改。

发布后，RT 仍只读取独立的值数据包；GT 可以继续修改下一帧。没有引入 RT 借用 World 存储的 freeze/pull/ack 协议，也没有按 flight 复制完整 Scene。注销、句柄复用、重连、重挂接、重复 Collect 与创建/删除相消均保留并验证。

## 哪些是框架成本，哪些工作仍须完成

- 本版减少的是变更收集中的组件指针访问、排序、重复打包、空通知树遍历，以及更新定位信息的回扫。这些属于框架组织成本。不同改动同时存在，本次没有做消融实验，不能把总收益精确分摊给某一项。
- 改变 10 万个 local 值仍要处理 10 万份数据。协议没有压缩：仅 local 更新载荷仍为约 4.8 MB/帧（48 × 100000 字节）。独立快照发布、RT 的增量依赖处理、世界矩阵与 bounds 更新仍有成本。
- 层级根移动只需发布根的 local，但所有受影响后代的世界矩阵和 bounds 仍需更新；这是渲染结果要求的工作。有业务订阅者时，对它们的通知也会恢复相应工作量。
- 全量随机更新的 6.545 ms 不能全部认定为不可消除的正常载荷：其中仍含随机组件/存储访问、队列和同步开销。要继续分摊阶段占比，需要针对新版本重新做分阶段 profile。本次吞吐结果只支持总路径比较。

## 代价与接口变化

- `OnTransformChanged` 默认关闭。覆盖该回调的业务组件必须显式调用 `SetTransformNotificationEnabled(true)`；仓库内相关测试组件已迁移。全量业务订阅的性能没有单独测量，不能套用无业务回调场景的收益。
- `GetRelativeLocation/Rotation/Scale` 现在返回值快照，不再返回内部 Eigen 引用。调用方若依赖引用身份或地址，需要调整。原来的按值读取和 setter 使用方式仍可用。
- 同值场景每帧对 100 个对象各执行 100 次 getter + setter，共 10000 次。双线程中位数 17.842 → 21.314 μs（+19.5%，绝对增加 3.472 μs）；没有生成变换更新。这反映访问/比较路径的代价，尚未用消融实验区分各因素。
- 突发替换场景每 32 帧替换 1000/10000 个对象：双线程平均 17.605 → 19.373 μs/帧（+10.0%）。这不是突发帧 P95/P99；这里没有逐帧延迟样本。开放世界流式场景中位数增加 5.1%，两组范围有重叠。
- 10 万对象只动 1 个时，单线程 0.249 → 0.292 μs，增加约 43 ns；双线程百分比更容易受调度噪声影响。不要把亚微秒场景的比例放大解读。
- 存储容量增长需 O(槽位数) 修正缓存指针；这里测的是初始化后的帧循环，未单独测量首次加载、扩容尖峰。持续创建/删除、流式与重挂接包含在帧负载中。World 的即时世界坐标查询仍为 O(深度)，未在此版本增加缓存。
- 批量 API 的正确性已验证，但以下性能数据全部保持原始组件 setter 工作负载，没有用新接口改写测例来扩大收益。WorldTransformId 仅在所属 World 内有效，不能跨 World 传递。

## 测量方法与边界

- 原版是开始本轮时的实际工作树，HEAD 为 `5c10ae4eedb3e9e94ffa1c25d3dbca415823ded7`，保留当时已有未提交改动；不是把工作树回退到纯 HEAD。原版二进制与源码已留档。
- 本机 Intel Core i7-13700K，Windows 11 build 26200，高性能电源计划。ClangCL 22.1.3、Release、AVX2，同一构建配置/allocator；RADRAY_ENABLE_PROFILER=OFF，避免 Tracy 开销混入。两个后端均开启，但此 CPU 测例不创建真实 GPU 绘制负载。
- Google Benchmark v1.9.5，原版/新版逐轮 AB、BA 交替串行运行，进程 affinity 统一为 `0xFFFF`（逻辑 CPU 0–15，未据此假设核心类型）。测量期间不并行编译、测试或运行其他基准。
- 每个 native case 使用进程 CPU 计时及墙钟计时；每批 256 帧并 drain 全部 flight。范围为 World 变更、Tick/Finalize、Collect/Seal、CPU 场景 Apply、CPU retirement；不包含初始化，也不包含 GPU 绘制、GPU upload 或视图渲染。
- 主比较：23 场景 × 单/双线程 × 5 轮，共 230 对原版/新版样本；F=2，min_time=0.2s、warmup=0.1s。表中是五个独立进程结果的中位数，不是逐帧 P50；CV 和范围也来自这五个结果。
- 扫描：全部 55 场景 × 单/双线程 × 1 轮，F=2，min_time=0.15s、warmup=0.05s。用于覆盖检查和寻找回退，单轮比例不作为稳定幅度的证据。
- flight 对照：5 场景 × F=1/3 × 单/双线程 × 3 轮，min_time=0.15s、warmup=0.05s。F=1 基本串行交接，F=2/3 允许流水重叠。
- 所有阶段使用相同最终候选二进制，命令、退出状态、原始 native JSON 和 SHA-256 均保存在对应 metadata 中。随机对象选择种子及 workload 源码未修改。
- 不能将本表绝对耗时直接与之前启用 Tracy 的 framework_stress GPU 帧耗时相减，也不能由同步路径加速比例推算实际游戏 FPS。

## 正确性与构建

Debug 580/580，Release 578/578 测试通过，失败/跳过均为零；测试环境要求 D3D12、Vulkan 两个后端。完整日志与 JUnit XML 已保留。

新增覆盖：存储扩容后数据有效、负缩放与旋转矩阵结果、bulk 全批验证、注销 generation 失效、槽位复用、渲染重连、重复 Collect 合并、封包后 GT 修改不影响旧包、空闲不输出，以及通知订阅启停/重挂接/父节点删除。原有生命周期、重入、跨线程和场景同步测试继续通过。

## 五轮完整结果

CV 是五个运行均值的变异系数。近似持平且范围重叠的场景不作显著改善/恶化判断；比如巡游新版一轮为 17.742 μs，其余结果接近原版，所以同时保留原始样本。完整 min/max 与每轮数据见 [结果数据](world-transform-store-results-2026-09-24.json)。

| 场景 / flight / 执行方式 | 原版 μs/帧 | 新版 μs/帧 | 耗时变化 | 原版 / 新版 CV |
|---|---:|---:|---:|---:|
| `shape_10000_dirty_1/F2/single` | 0.230 | 0.235 | +1.8% | 2.9% / 6.0% |
| `shape_10000_dirty_1/F2/threaded` | 0.463 | 0.469 | +1.3% | 8.8% / 8.0% |
| `shape_10000_dirty_100/F2/single` | 6.361 | 5.941 | -6.6% | 3.6% / 6.0% |
| `shape_10000_dirty_100/F2/threaded` | 3.311 | 3.274 | -1.1% | 3.0% / 8.2% |
| `shape_10000_dirty_1000/F2/single` | 51.237 | 49.153 | -4.1% | 2.5% / 12.6% |
| `shape_10000_dirty_1000/F2/threaded` | 25.458 | 24.510 | -3.7% | 2.3% / 4.9% |
| `shape_10000_dirty_10000/F2/single` | 511.328 | 344.866 | -32.6% | 0.9% / 3.5% |
| `shape_10000_dirty_10000/F2/threaded` | 330.170 | 183.167 | -44.5% | 1.4% / 1.1% |
| `shape_100000_dirty_0/F2/single` | 0.103 | 0.104 | +1.2% | 0.8% / 1.9% |
| `shape_100000_dirty_0/F2/threaded` | 0.365 | 0.355 | -2.8% | 14.3% / 8.3% |
| `shape_100000_dirty_1/F2/single` | 0.249 | 0.292 | +17.3% | 5.1% / 7.5% |
| `shape_100000_dirty_1/F2/threaded` | 0.447 | 0.483 | +8.1% | 11.1% / 11.7% |
| `shape_100000_dirty_1000/F2/single` | 113.701 | 114.189 | +0.4% | 1.9% / 1.9% |
| `shape_100000_dirty_1000/F2/threaded` | 46.631 | 48.566 | +4.2% | 6.7% / 4.4% |
| `shape_100000_dirty_10000/F2/single` | 1666.713 | 1341.329 | -19.5% | 7.4% / 7.1% |
| `shape_100000_dirty_10000/F2/threaded` | 680.696 | 639.231 | -6.1% | 4.0% / 10.1% |
| `shape_100000_dirty_100000/F2/single` | 17563.543 | 9006.880 | -48.7% | 1.1% / 1.9% |
| `shape_100000_dirty_100000/F2/threaded` | 15464.152 | 6545.305 | -57.7% | 1.2% / 1.5% |
| `shape_100000_sequential_all/F2/single` | 10568.183 | 4293.621 | -59.4% | 1.5% / 4.0% |
| `shape_100000_sequential_all/F2/threaded` | 8509.065 | 2359.009 | -72.3% | 2.6% / 1.6% |
| `repeat_100/F2/single` | 81.771 | 70.801 | -13.4% | 2.3% / 5.4% |
| `repeat_100/F2/threaded` | 77.902 | 66.702 | -14.4% | 1.0% / 1.7% |
| `same_value_100/F2/single` | 16.113 | 19.642 | +21.9% | 1.6% / 1.1% |
| `same_value_100/F2/threaded` | 17.842 | 21.314 | +19.5% | 1.7% / 1.8% |
| `chain_16_root/F2/single` | 57.180 | 26.708 | -53.3% | 2.7% / 0.9% |
| `chain_16_root/F2/threaded` | 32.762 | 24.418 | -25.5% | 2.6% / 1.7% |
| `wide_16_root/F2/single` | 43.973 | 27.691 | -37.0% | 1.9% / 1.9% |
| `wide_16_root/F2/threaded` | 25.853 | 25.013 | -3.2% | 4.6% / 1.9% |
| `churn_10/F2/single` | 15.913 | 16.719 | +5.1% | 3.6% / 4.5% |
| `churn_10/F2/threaded` | 17.546 | 17.694 | +0.8% | 2.4% / 5.7% |
| `burst_replace_10pct/F2/single` | 15.524 | 17.045 | +9.8% | 1.9% / 4.9% |
| `burst_replace_10pct/F2/threaded` | 17.605 | 19.373 | +10.0% | 4.0% / 2.8% |
| `reparent_subtrees/F2/single` | 70.293 | 40.296 | -42.7% | 0.5% / 1.8% |
| `reparent_subtrees/F2/threaded` | 41.512 | 28.341 | -31.7% | 1.7% / 1.9% |
| `mixed/F2/single` | 44.060 | 40.488 | -8.1% | 1.8% / 10.2% |
| `mixed/F2/threaded` | 41.257 | 35.367 | -14.3% | 3.5% / 8.3% |
| `level_walkthrough/F2/single` | 22.521 | 23.246 | +3.2% | 3.1% / 6.8% |
| `level_walkthrough/F2/threaded` | 12.321 | 12.257 | -0.5% | 3.3% / 18.3% |
| `level_firefight/F2/single` | 196.339 | 199.184 | +1.4% | 0.9% / 2.6% |
| `level_firefight/F2/threaded` | 115.118 | 116.263 | +1.0% | 6.7% / 2.2% |
| `open_world_streaming/F2/single` | 68.418 | 70.661 | +3.3% | 6.7% / 4.1% |
| `open_world_streaming/F2/threaded` | 65.093 | 68.423 | +5.1% | 4.6% / 6.7% |
| `crowd_animation/F2/single` | 48.168 | 38.127 | -20.8% | 2.1% / 0.9% |
| `crowd_animation/F2/threaded` | 31.362 | 30.708 | -2.1% | 1.8% / 3.4% |
| `editor_idle/F2/single` | 0.357 | 0.302 | -15.4% | 10.3% / 9.1% |
| `editor_idle/F2/threaded` | 0.554 | 0.488 | -11.8% | 5.9% / 6.8% |

## F=1 / F=3 三轮对照

10 万对象全量随机更新的双线程耗时在 F=1 降低 49.2%，F=3 降低 59.5%；密集更新收益不依赖 F=2。巡游与稀疏更新依旧没有普遍改善。

| 场景 / flight / 执行方式 | 原版 μs/帧 | 新版 μs/帧 | 耗时变化 | 原版 / 新版 CV |
|---|---:|---:|---:|---:|
| `shape_100000_dirty_1000/F1/single` | 111.423 | 111.130 | -0.3% | 5.5% / 3.8% |
| `shape_100000_dirty_1000/F1/threaded` | 97.901 | 102.750 | +5.0% | 4.0% / 1.6% |
| `shape_100000_dirty_1000/F3/single` | 110.904 | 112.028 | +1.0% | 2.1% / 3.1% |
| `shape_100000_dirty_1000/F3/threaded` | 44.585 | 44.458 | -0.3% | 4.9% / 1.2% |
| `shape_100000_dirty_100000/F1/single` | 17620.493 | 8831.741 | -49.9% | 2.6% / 2.1% |
| `shape_100000_dirty_100000/F1/threaded` | 17522.655 | 8894.503 | -49.2% | 0.5% / 2.0% |
| `shape_100000_dirty_100000/F3/single` | 17661.963 | 9045.920 | -48.8% | 2.5% / 4.8% |
| `shape_100000_dirty_100000/F3/threaded` | 15554.898 | 6296.448 | -59.5% | 1.4% / 0.5% |
| `chain_16_root/F1/single` | 58.159 | 26.323 | -54.7% | 0.8% / 0.9% |
| `chain_16_root/F1/threaded` | 69.858 | 38.094 | -45.5% | 2.0% / 2.7% |
| `chain_16_root/F3/single` | 58.089 | 26.838 | -53.8% | 1.3% / 1.2% |
| `chain_16_root/F3/threaded` | 31.933 | 23.835 | -25.4% | 9.1% / 6.1% |
| `reparent_subtrees/F1/single` | 69.332 | 39.427 | -43.1% | 2.3% / 1.6% |
| `reparent_subtrees/F1/threaded` | 81.571 | 49.399 | -39.4% | 3.8% / 3.5% |
| `reparent_subtrees/F3/single` | 72.874 | 39.655 | -45.6% | 3.7% / 2.1% |
| `reparent_subtrees/F3/threaded` | 40.788 | 27.910 | -31.6% | 1.2% / 0.2% |
| `level_walkthrough/F1/single` | 22.107 | 23.201 | +4.9% | 7.7% / 3.0% |
| `level_walkthrough/F1/threaded` | 35.337 | 36.460 | +3.2% | 8.0% / 14.7% |
| `level_walkthrough/F3/single` | 23.639 | 23.630 | -0.0% | 1.9% / 8.7% |
| `level_walkthrough/F3/threaded` | 11.337 | 11.303 | -0.3% | 3.0% / 7.2% |

## 全部 F=2 场景的一轮覆盖

| 场景 / flight / 执行方式 | 原版 μs/帧 | 新版 μs/帧 | 耗时变化 |
|---|---:|---:|---:|
| `shape_1000_dirty_0/F2/single` | 0.104 | 0.104 | -0.3% |
| `shape_1000_dirty_0/F2/threaded` | 0.337 | 0.326 | -3.2% |
| `shape_1000_dirty_1/F2/single` | 0.159 | 0.150 | -5.9% |
| `shape_1000_dirty_1/F2/threaded` | 0.391 | 0.417 | +6.5% |
| `shape_1000_dirty_10/F2/single` | 0.497 | 0.478 | -3.7% |
| `shape_1000_dirty_10/F2/threaded` | 0.730 | 0.607 | -16.9% |
| `shape_1000_dirty_100/F2/single` | 3.832 | 3.603 | -6.0% |
| `shape_1000_dirty_100/F2/threaded` | 3.460 | 2.261 | -34.7% |
| `shape_1000_dirty_1000/F2/single` | 34.008 | 27.749 | -18.4% |
| `shape_1000_dirty_1000/F2/threaded` | 24.933 | 19.242 | -22.8% |
| `shape_10000_dirty_0/F2/single` | 0.105 | 0.104 | -0.8% |
| `shape_10000_dirty_0/F2/threaded` | 0.397 | 0.368 | -7.2% |
| `shape_10000_dirty_1/F2/single` | 0.231 | 0.223 | -3.7% |
| `shape_10000_dirty_1/F2/threaded` | 0.523 | 0.433 | -17.2% |
| `shape_10000_dirty_100/F2/single` | 6.056 | 5.986 | -1.2% |
| `shape_10000_dirty_100/F2/threaded` | 3.424 | 3.208 | -6.3% |
| `shape_10000_dirty_1000/F2/single` | 46.723 | 49.839 | +6.7% |
| `shape_10000_dirty_1000/F2/threaded` | 25.529 | 25.901 | +1.5% |
| `shape_10000_dirty_10000/F2/single` | 498.513 | 347.483 | -30.3% |
| `shape_10000_dirty_10000/F2/threaded` | 349.464 | 190.529 | -45.5% |
| `shape_100000_dirty_0/F2/single` | 0.103 | 0.104 | +1.1% |
| `shape_100000_dirty_0/F2/threaded` | 0.373 | 0.355 | -4.7% |
| `shape_100000_dirty_1/F2/single` | 0.248 | 0.315 | +27.3% |
| `shape_100000_dirty_1/F2/threaded` | 0.510 | 0.565 | +10.8% |
| `shape_100000_dirty_1000/F2/single` | 112.567 | 109.057 | -3.1% |
| `shape_100000_dirty_1000/F2/threaded` | 47.931 | 48.684 | +1.6% |
| `shape_100000_dirty_10000/F2/single` | 1548.160 | 1333.191 | -13.9% |
| `shape_100000_dirty_10000/F2/threaded` | 664.258 | 625.742 | -5.8% |
| `shape_100000_dirty_100000/F2/single` | 17520.470 | 9114.127 | -48.0% |
| `shape_100000_dirty_100000/F2/threaded` | 15261.137 | 6405.054 | -58.0% |
| `shape_10000_sequential_all/F2/single` | 422.323 | 278.170 | -34.1% |
| `shape_10000_sequential_all/F2/threaded` | 287.514 | 172.195 | -40.1% |
| `shape_100000_sequential_all/F2/single` | 10506.739 | 4141.000 | -60.6% |
| `shape_100000_sequential_all/F2/threaded` | 8503.663 | 2322.682 | -72.7% |
| `repeat_1/F2/single` | 6.424 | 6.460 | +0.6% |
| `repeat_1/F2/threaded` | 3.343 | 3.211 | -4.0% |
| `repeat_10/F2/single` | 13.601 | 12.894 | -5.2% |
| `repeat_10/F2/threaded` | 11.110 | 10.279 | -7.5% |
| `repeat_100/F2/single` | 80.970 | 69.586 | -14.1% |
| `repeat_100/F2/threaded` | 77.538 | 67.468 | -13.0% |
| `same_value_100/F2/single` | 15.851 | 19.543 | +23.3% |
| `same_value_100/F2/threaded` | 17.765 | 21.045 | +18.5% |
| `chain_1_leaf/F2/single` | 6.535 | 5.924 | -9.3% |
| `chain_1_leaf/F2/threaded` | 3.364 | 3.198 | -4.9% |
| `chain_1_root/F2/single` | 6.427 | 5.977 | -7.0% |
| `chain_1_root/F2/threaded` | 3.418 | 3.270 | -4.3% |
| `chain_4_leaf/F2/single` | 8.294 | 7.339 | -11.5% |
| `chain_4_leaf/F2/threaded` | 4.022 | 3.702 | -8.0% |
| `chain_4_root/F2/single` | 16.593 | 10.974 | -33.9% |
| `chain_4_root/F2/threaded` | 8.288 | 8.543 | +3.1% |
| `chain_16_leaf/F2/single` | 12.546 | 5.012 | -60.1% |
| `chain_16_leaf/F2/threaded` | 4.992 | 3.156 | -36.8% |
| `chain_16_root/F2/single` | 56.818 | 26.636 | -53.1% |
| `chain_16_root/F2/threaded` | 33.023 | 24.105 | -27.0% |
| `wide_4_root/F2/single` | 15.761 | 10.570 | -32.9% |
| `wide_4_root/F2/threaded` | 7.873 | 8.642 | +9.8% |
| `wide_16_root/F2/single` | 43.592 | 27.746 | -36.3% |
| `wide_16_root/F2/threaded` | 25.650 | 25.418 | -0.9% |
| `rebind_ready/F2/single` | 7.110 | 6.677 | -6.1% |
| `rebind_ready/F2/threaded` | 4.860 | 4.767 | -1.9% |
| `light_1_clean/F2/single` | 0.103 | 0.104 | +0.8% |
| `light_1_clean/F2/threaded` | 0.403 | 0.370 | -8.3% |
| `light_1_one/F2/single` | 0.242 | 0.242 | +0.2% |
| `light_1_one/F2/threaded` | 0.574 | 0.493 | -14.1% |
| `light_1_all/F2/single` | 0.245 | 0.242 | -1.5% |
| `light_1_all/F2/threaded` | 0.487 | 0.494 | +1.5% |
| `light_16_clean/F2/single` | 0.103 | 0.104 | +1.3% |
| `light_16_clean/F2/threaded` | 0.350 | 0.392 | +11.8% |
| `light_16_one/F2/single` | 0.363 | 0.361 | -0.6% |
| `light_16_one/F2/threaded` | 0.674 | 0.665 | -1.3% |
| `light_16_all/F2/single` | 1.874 | 1.804 | -3.7% |
| `light_16_all/F2/threaded` | 1.645 | 1.662 | +1.0% |
| `light_64_clean/F2/single` | 0.104 | 0.104 | -0.5% |
| `light_64_clean/F2/threaded` | 0.371 | 0.349 | -6.1% |
| `light_64_one/F2/single` | 0.646 | 0.649 | +0.5% |
| `light_64_one/F2/threaded` | 0.991 | 0.946 | -4.6% |
| `light_64_all/F2/single` | 7.094 | 7.015 | -1.1% |
| `light_64_all/F2/threaded` | 5.627 | 5.629 | +0.0% |
| `light_256_clean/F2/single` | 0.103 | 0.103 | -0.3% |
| `light_256_clean/F2/threaded` | 0.365 | 0.404 | +10.7% |
| `light_256_one/F2/single` | 1.903 | 1.953 | +2.6% |
| `light_256_one/F2/threaded` | 2.068 | 1.974 | -4.5% |
| `light_256_all/F2/single` | 28.855 | 27.896 | -3.3% |
| `light_256_all/F2/threaded` | 23.109 | 22.982 | -0.5% |
| `churn_10/F2/single` | 15.718 | 16.502 | +5.0% |
| `churn_10/F2/threaded` | 16.989 | 16.901 | -0.5% |
| `burst_replace_10pct/F2/single` | 15.567 | 16.064 | +3.2% |
| `burst_replace_10pct/F2/threaded` | 16.295 | 17.976 | +10.3% |
| `reparent/F2/single` | 15.700 | 17.533 | +11.7% |
| `reparent/F2/threaded` | 11.393 | 11.429 | +0.3% |
| `reparent_all/F2/single` | 1463.586 | 1458.718 | -0.3% |
| `reparent_all/F2/threaded` | 1189.782 | 1168.675 | -1.8% |
| `reparent_subtrees/F2/single` | 69.481 | 39.668 | -42.9% |
| `reparent_subtrees/F2/threaded` | 42.542 | 28.268 | -33.6% |
| `chain_16_mixed/F2/single` | 288.136 | 134.463 | -53.3% |
| `chain_16_mixed/F2/threaded` | 165.927 | 106.969 | -35.5% |
| `mixed/F2/single` | 47.262 | 40.303 | -14.7% |
| `mixed/F2/threaded` | 39.056 | 34.570 | -11.5% |
| `level_walkthrough/F2/single` | 23.276 | 22.713 | -2.4% |
| `level_walkthrough/F2/threaded` | 11.728 | 12.474 | +6.4% |
| `level_firefight/F2/single` | 195.952 | 195.996 | +0.0% |
| `level_firefight/F2/threaded` | 108.301 | 115.742 | +6.9% |
| `open_world_streaming/F2/single` | 65.642 | 68.671 | +4.6% |
| `open_world_streaming/F2/threaded` | 61.346 | 66.639 | +8.6% |
| `crowd_animation/F2/single` | 46.812 | 38.278 | -18.2% |
| `crowd_animation/F2/threaded` | 30.719 | 31.087 | +1.2% |
| `cinematic_lights/F2/single` | 45.525 | 40.631 | -10.7% |
| `cinematic_lights/F2/threaded` | 29.074 | 28.580 | -1.7% |
| `editor_idle/F2/single` | 0.338 | 0.302 | -10.5% |
| `editor_idle/F2/threaded` | 0.612 | 0.497 | -18.8% |

## 留档与复现

[随文结果 JSON](world-transform-store-results-2026-09-24.json) 保存三个正式阶段的全部比较、每轮数值、min/max、CV、CPU 时间、命令、退出状态、时间戳与二进制 SHA-256；阅读本文不依赖本机 artifacts。

本机额外留档根目录为 `artifacts/transform_store_20260924`，被 Git 忽略：`baseline/sources`、`prototype_v3/sources` 保存前后源码；各自 `source.patch` 保存相对 Git HEAD 的完整差异（包含开始时用户已有改动）；`implementation.patch` 仅含本轮 C++ 改动。该目录还保存原始 native JSON、构建/测试日志与测量脚本。`quick*`、`sweep`、`prototype` 是开发中间版本，不能替代本报告的最终结果。

构建与原生基准用法见[构建与测试](../guide/build-test.md#world--renderscene-状态同步基准)。比较新旧版时先分别保留 Release 二进制，再固定 affinity 串行交替执行；原生示例（路径需替换为对应版本）：

```powershell
./build_transform_perf/_build/Release/bench_scene_sync.exe `
  --benchmark_filter='SceneSync/(shape_100000_dirty_100000|level_walkthrough)/F2/' `
  --benchmark_min_time=0.2s --benchmark_min_warmup_time=0.1 `
  --benchmark_repetitions=1 --benchmark_out=result.json --benchmark_out_format=json
```
