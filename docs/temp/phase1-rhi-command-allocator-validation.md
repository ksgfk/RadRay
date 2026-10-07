> - 适用: 2026-10-07 第一阶段 RHI CommandAllocator、公共 pipeline 与材质绘制实现的本机验收
> - 权威: 临时实施与测量记录，不替代长期接口契约；未运行项不计为通过
> - 锚点: `modules/render/tests/test_command_allocator.cpp`, `modules/runtime/tests/test_pipeline.cpp`, `modules/runtime/tests/test_scene_draw.cpp`, `benchmarks/bench_command_allocator/bench_command_allocator.cpp`

# 第一阶段实施与验收记录

基线为 `eb4f1f4d2a4479af2f2f83b017c33af0be80c913`，分支 `refactor/render-framework-reset`。
记录对应在此基线上的未提交工作树；原始输入为仓库根的 `RadRay_Phase1_Plan_RHI_CommandAllocator.md`。
公共实现和调用方迁移已落地，下述实测覆盖不能等同原计划每个矩阵格都已独立验证。

## 实施范围与当前边界

- RHI 显式 `CommandAllocator` 绑定实际 queue；删除旧 queue 参数的 CommandBuffer 工厂。
  D3D12 按同时打开的 command list 高水位共享 native allocator；Vulkan 同域共用一个 flags=0 的 pool。
  Begin 不重置共享存储，Reset 清理全部 child 的旧录制与附属状态。child 先于父 allocator 销毁。
- `GpuFlightCommandAllocator` 直接实现 `ICmdAllocator`；Return 仅关闭，结果数组规定顺序，
  `RegisterClosedCommandBuffers` 登记批次。旧 AppFrameContext Return 入口仍适配关闭与登记。
  profiler 和 late-drop cleanup 继续在同一 flight storage 续录，由既有 final fence 覆盖。
- `PipelineContext`、`RenderPipelineRequest/Result`、实际附件驱动的 RenderOutput 和失败收尾已接入。
  返回的状态是录制计划；实际执行仍由原 completion 协议确认。无 Scene、无输出的自定义入口可用。
- 新增不可变 Material、typed material 更新、材质纹理保活、section slot、拥有 semantic 的多流顶点布局，
  以及按 program/input 内容复用的匹配与绑定解释。transform-only 与 material-only 分开交付。
- RenderSystem 拥有 PSO 内容缓存；per-flight 常量页和 layout/group 参数池在安全复用点重置。
  公共 SceneDraw 在准备阶段创建资源，draw 循环使用 ShapeId.Index，保留负缩放绕序。
  相邻相同 geometry/material 使用局部缓存避免重复查表，不排序、不合批、不减少 draw 数。
- scene_sync 与 framework_stress 默认调用 UnlitRenderPipeline；depth-only 通过不同 pass 契约使用同一公共绘制器。
  支持透视/正交与超过三个视图，旧固定绘制循环只由压测 `--legacy-draw` 使用。

第一阶段仍不包含 RenderGraph、剔除、instancing、间接绘制、异步队列或共享 pool 的并发录制。
当前顶点匹配支持 32 位 float/sint/uint 的 1–4 分量。材质构建要求纹理 Ready，常量 bytes 由调用方遵守 shader 布局。
输出限定单 mip、单 layer、附件尺寸和 sample count 相同；设备创建与 RHI 验证继续拒绝不支持的格式/能力。
RHI 没有公开 Texture 的所属设备查询，RenderOutput 不额外宣称能检测跨设备附件。
缓存旧 program/PSO 在 idle 拆除；未实现细粒度热重载回收。

长期契约见 [RHI](../architecture/render-rhi.md)、[帧与 GPU](../architecture/frame-and-gpu.md)
及[渲染框架](../architecture/render-framework.md)。

## 环境与正确性验证

本机 Windows 11 Pro 10.0.26300，Intel Core i7-13700K，NVIDIA RTX 4080，驱动 32.0.16.1714。
CMake 4.4.4、Visual Studio 18 2026 的 ClangCL/Clang 22.1.3；D3D12、Vulkan、shader JIT、mimalloc 和编译期 profiler 均开启。
GPU 测试要求 `RADRAY_TEST_REQUIRED_BACKENDS=d3d12,vulkan`，并使用 fixture 的验证层设置。
性能测试关闭 native validation，没有连接 Tracy viewer。

| 检查 | 实际结果 |
|---|---|
| Debug / Release 构建 | 两配置成功；包含 render/runtime tests、样例、benchmark |
| Release 全量 CTest | 601/601 通过，159.44 秒；没有实际跳过的测试 |
| 最终参数池、绑定检查与循环查表修改后的相关回归 | `SceneDraw\.|Pipeline|SceneAssets\.`，Debug 44/44（22.80 秒），Release 44/44（21.95 秒） |
| scene_sync 窗口 smoke | D3D12 F=1、7 views；Vulkan GT/RT、F=3、7 views；各 200 objects、40 frames、validation 开启，退出码 0、日志无错误 |
| 文档检查 | `python tools/check_docs.py`、`git diff --check` |

Release 全量验证先于最后的局部准备/参数池优化；该优化的受影响用例随后在双配置复测。
初次 Debug 全量曾有两个材质用例失败：运行期间修改了 shader 输入、旧二进制仍按旧布局构建。
重新构建并固定源码后，材质与完整相关测试通过；没有把那次 Debug 全量写为通过。
本地原始日志在 `build_release/phase1-full-tests.log`、两构建目录的 `phase1-acceptance-tests.log`
和 `build_release/phase1-window-*.log`；构建目录为被忽略的本机产物，不是可移植交付附件。

### 已执行的关键机制覆盖

| 范围 | 证据 |
|---|---|
| 存储共享、重叠与扩容 | 两后端 M=1/8/32/128、K=1/2/4，GPU copy/readback；D3D12 检查 native 容量，Vulkan 检查全部 child 借用同一 pool |
| Reset 与 child 生命周期 | 完成后复用、销毁一个 child 后继续使用兄弟、幂等 Destroy、64 个旧 encoder 的整批清理 |
| 非法录制状态 | recording 时 Reset、重复 Begin/End、未结束 encoder、父提前销毁、Reset 后旧录制 Submit 的 death tests |
| 返回/登记/提交 | 逆序 Return、显式依赖顺序 Submit、重复/未关闭登记、未归还、flight 未退休复用拒绝 |
| 内部续录与完成 | 两后端 profiler 开/关、有序批次、final-fence-only retirement、既有 multi-window/late-drop 回归 |
| 输出/独立 pipeline | CPU fixture 验证无 Scene/无输出、无工作收尾、已有命令后的失败收尾顺序、重叠 attachment alias 拒绝 |
| 几何/材质与参数 | 拥有 semantic、非法布局、交错与多流、TEXCOORD1/location 3、混合物理 group、不同常量/纹理、typed 更新合并、stale generation、两 Scene 共享依赖 |
| 绘制与视图 | 两后端 GPU 像素读回，单/双线程，0/1/3/7 views、正交、透视、depth-only、resize、相机-only、late-drop retry |
| 缓存/帧资源 | 内容 key 与 hash 冲突、参数变化时 PSO 数不增加、常量对齐/页增长/切片不覆盖、terminal 参数资源先于 layout 销毁 |

## CommandAllocator 专项测量

`bench_command_allocator` 比较当前 API 下每 command 一份 storage 与一份共享 storage，
两组保持相同 wrapper 缓存、M、K、每条 4-byte copy 录制，不提交 GPU。
初始化及首次 native 扩容排除在计时外；手动计时拆成 Reset 与 Begin/copy/End。
这不是旧提交二进制的端到端比较，也不包含 GPU 执行和 flight 等待。

完整矩阵为两个后端 × 两种 storage × 10 组 M/K，共 40 个场景，每场景 5 次；
[原始样本与汇总](phase1-command-allocator-samples.csv) 共 360 行。
未固定 CPU 的样本离散较大，M=1 两组也有偏差，因此不据此宣称固定速度比。

补充固定主线程到 logical CPU 0，warmup 0.2 秒、min-time 0.2 秒、7 repetitions，
只复测 M=1/32、K=1 的 8 个场景；[原始样本](phase1-command-allocator-pinned.csv) 共 88 行。
下表为每次 iteration 样本的中位数，单位 μs；分项分别取中位数，和不必精确等于总计。

| 后端 | M | storage | 总计 | Reset | Record |
|---|---:|---|---:|---:|---:|
| D3D12 | 1 | per-command | 3.019 | 0.725 | 2.305 |
| D3D12 | 1 | shared | 3.041 | 0.730 | 2.296 |
| D3D12 | 32 | per-command | 112.259 | 23.954 | 88.662 |
| D3D12 | 32 | shared | 76.560 | 1.134 | 75.426 |
| Vulkan | 1 | per-command | 0.538 | 0.145 | 0.393 |
| Vulkan | 1 | shared | 0.530 | 0.143 | 0.387 |
| Vulkan | 32 | per-command | 18.583 | 4.613 | 13.907 |
| Vulkan | 32 | shared | 17.187 | 3.593 | 13.594 |

全部样本的 native capacity 符合 per-command=M、shared D3D12=K、shared Vulkan=1；
GPU correctness 还执行了高水位增长后退回单 command 的复用。
native 容量是直接读取的现有后端状态，不是新增生产统计字段。
Reset 实现仅循环已使用 D3D12 blocks 或执行一次 Vulkan pool Reset，无 Wait。
本次没有注入原生 API 拦截器：create/reset/free 调用数是代码路径结论，不能当作逐调用实测计数。
相应 native command storage 字节数无法直接测得。

复现矩阵：

```powershell
build_release/_build/Release/bench_command_allocator.exe --benchmark_min_time=0.1s --benchmark_repetitions=5 --benchmark_out=build_release/command-allocator.json --benchmark_out_format=json
```

## 真实绘制 A/B

公共 pipeline 与 `--legacy-draw` 都使用当前 RHI allocator；这组比较反映公共准备/绘制的接入成本。
同一 cube、固定 shader、单 view、F=2、单线程、1280×720 离屏、无深度、无排序/剔除，
每帧 100 个移动对象，120 帧 warmup 后 500 帧；进程 affinity=0xffff（logical CPU 0–15）。
每组 3 次、交替顺序，分别测试运行时 GPU profiler 开/关；编译期 CPU profiler 始终开启。

时间取日志 `warmup complete` 到 `finished` 的 wall time 除以 500，包含退出前 drain。
它是平均帧耗时，不能当 CPU-only、GPU-only 或逐帧 P95/P99。
工作集由外部进程每 20 ms 采样 `GetProcessMemoryInfo`；峰值包含启动、DXC、驱动和分配器，
另记录 warmup 后观察到的最大当前 working set。采样会漏掉短时峰值。
命令与每次结果保存在 CSV，表内给出三次中位数和 min–max。

### 10,000 objects

原始数据：[phase1-scene-samples.csv](phase1-scene-samples.csv)。

| 后端 | GPU profiler | legacy ms（min–max） | pipeline ms（min–max） | legacy / pipeline 峰值 MiB |
|---|---|---:|---:|---:|
| D3D12 | off | 0.752（0.748–0.800） | 0.768（0.762–0.784） | 151.77 / 184.50 |
| D3D12 | on | 0.794（0.792–0.806） | 0.820（0.820–0.822） | 154.02 / 186.26 |
| Vulkan | off | 1.434（1.424–1.438） | 1.440（1.424–1.442） | 135.40 / 135.33 |
| Vulkan | on | 1.450（1.436–1.462） | 1.444（1.444–1.446） | 167.74 / 167.65 |

最初的准备和 draw 循环重复查询相同 geometry/material；
[优化前样本](phase1-scene-before-samples.csv) 中 D3D12 profiler-off 的 public/legacy 中位数为 1.036/0.786 ms，
Vulkan 为 1.716/1.480 ms。移除相邻相同组合的重复查表、延迟创建空材质的等待 scope 后，
固定负载的主要额外开销下降；参数池另按 layout/group 缓存，避免随版本数线性扫描。
这些对照不能精确拆分三个优化各自的收益；没有改变 draw 数或变换算法。

D3D12 pipeline 在 10k 场景的峰值工作集高约 32 MiB，warmup 后仍可观察到，
不能把 native allocator 数减少等同进程内存下降。常量 arena 默认每页 256 KiB，
本场景 F=2 的首批页请求约 0.5 MiB，单凭请求大小不能解释全部差额。
目前未将剩余差额归因到某个 driver/DXC/CPU allocator reservation，不宣称已定位内存差异。

### 100,000 objects

保持前述参数，仅将 `--objects=100000`；每帧仍移动 100 个对象，仍逐对象绘制，
没有通过增加深度测试、排序或剔除改变旧绘制语义。
原始数据：[phase1-scene100k-samples.csv](phase1-scene100k-samples.csv)。

| 后端 | GPU profiler | legacy ms（min–max） | pipeline ms（min–max） | legacy / pipeline 峰值 MiB |
|---|---|---:|---:|---:|
| D3D12 | off | 6.288（6.262–6.308） | 6.652（6.634–6.656） | 289.02 / 321.11 |
| D3D12 | on | 6.438（6.272–6.524） | 6.634（6.630–6.690） | 290.63 / 322.65 |
| Vulkan | off | 14.862（14.546–14.910） | 14.922（14.804–14.936） | 314.32 / 305.25 |
| Vulkan | on | 14.710（14.686–14.750） | 14.802（14.768–14.822） | 346.01 / 339.64 |

D3D12 profiler-off 的差值为 0.364 ms（约 5.8%），超过这组三次重复的离散范围。
公共路径增加了准备阶段的线性场景/section 遍历，draw 时也要选择实例材质和准备结果；
固定 shader 的 legacy 只有单次绘制遍历。这些是源码中可确认的额外工作，但本次未独立计时，
不能把全部 0.364 ms 精确归因到其中一项。它不是 command storage 专项的速度比。
若后续需要继续优化，应先抓取准备与 draw 的分段时间，保留本阶段逐对象绘制的语义。
D3D12 工作集差额仍约 32 MiB，未随对象数量扩大十倍；这提示固定容量差异，尚不能确定来源或断言没有泄漏。

## 尚未覆盖与合并前注意项

- 本机只验证 Windows/NVIDIA；未验证其他厂商、非 Windows、编译期 profiler OFF、JIT OFF 配置。
- 未对原生 Create/Close/Reset 做故障注入，未单独执行两个独立 allocator 分别在 worker 上录制的 RALLOC-19。
  同域并发仍不在契约内。未单独拦截每条 native API 的 create/reset/free 次数。
- 自动化没有逐格覆盖原计划：例如独立的 2/4 views、同一相机同时宽屏与方形输出、
  全部 PSO 字段逐项变更与热重载在途版本、同一窗口多 pipeline 合并等，不能由现有通过数推断这些都已验证。
- CPU fixture 的无 Scene pipeline 验证了命令与失败收尾；没有另外添加真实 GPU 无 Scene compute pipeline 效果测试。
- 没有导出逐帧尾部分位、Scene Apply/参数准备/登记的独立耗时序列或 GPU 时间序列；已有 profiler zones 可用于后续抓取。
  样本是本机短时 A/B，不承诺跨设备性能。D3D12 工作集差额仍需更细的分配归因。
- D3D12 每 command 的 empty root signature 未提升为 device 共享资源（计划中的可选优化）。

代码路径与实测均未显示引入额外 GPU 等待；业务成功/失败仍不决定存储能否回收，
真实 completion 与所有权交接才允许下一次 BeginFrameRecord 重置 flight 资源。
