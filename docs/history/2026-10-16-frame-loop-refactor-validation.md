> - 适用: 2026-10-16 帧循环、事件派发和关停重构的实施验收
> - 权威: 本次工作树的执行记录与覆盖边界；不作为当前契约
> - 锚点: `modules/window/src/win32/win32_window.cpp`, `modules/window/tests/test_event_pump.cpp`, `docs/history/2026-10-21-runtime-archive.md`
归档维护注（2026-10-21）：本记录的原基线、结果及当时语义不变；下列源码锚点已随 runtime 移除，按原基线或归档说明查阅：`modules/runtime/src/application_loop.h`, `modules/runtime/src/application_loop.cpp`, `modules/runtime/src/application.cpp`, `modules/runtime/src/gpu_system.cpp`, `modules/runtime/tests/test_application_loop.cpp`。
正文中指向 runtime 的“当前”说明现仅为历史快照，不代表主线能力。


# 帧循环重构实施与验收

状态：已实施；当前 Windows 环境验收完成。既有跳过、未实测范围及文档全量检查限制见下。
范围：Win32、D3D12/Vulkan、单/双线程、CPU-only 与无窗口 Application。
基线：`93a81176a3bdc728cc0572c13deebba047a69251`，以及实施前已有的
`modules/runtime/include/radray/runtime/app_render_context.h` include 修改；该修改保留。
实施改动尚未提交，基线提交号不代表整个工作树。原设计与确认决策见[输入计划](2026-10-16-frame-loop-refactor-plan.md)。

## 初次实现（删除 ModalVBlank 前）

- Application 持有同名头源文件组成的 ApplicationLoop，统一正常帧、模态、维护和停止状态。
  最旧槽位形成背压；prepared 帧号保证 S0、计时、事件派发、Update、发布各一次。首帧计时起点在 OnInit 之后，保持原 DeltaTime 语义。
- RT 完成提交态写入后 release 发布 CPU 计数；GT acquire 后仅把已交付的连续身份范围传给
  RetireSubmittedFlights。FrameSerial 用于核验身份，主队列 final fence 值核验提交顺序；不假定提前保留的 serial 就是提交顺序。真实 GPU 完成前不退休；原同步接口保留给独立宿主。
- Win32 延迟 FIFO 拥有文本，冻结派发截止，注册身份不复用，注销/销毁取消旧事件。
  原生采集每次最多 256 条，进度事件和实际线程退出句柄与 QS_ALLINPUT 一起等待。
  GPU 背压使用 1 ms WaitFor 片段与不被消息重置的 30 秒截止。
- 维护/停止的临时线程只等待 final fence 和独立的主队列 idle；实际退出后 GT 才 join、retire 和交付。
  RT 先消费已发布包并退出，再 BeginStopping。模态 tick 可继续推进停止，但最终拆除只在外层循环。
  VBlank 辅助线程也按实际退出状态回收。OnInit 期间禁止模态入口提前推进；停止时取消原生菜单等模态状态，使消息栈可以返回。普通帧不新增线程。
- 退出取消普通输入；真实主窗口关闭在安全点交付一次，RequestExit 不伪造关闭事件。
  析构复用 Drain，不派发延迟游戏事件或调用派生 OnShutdown；Run 作用域清理在调用栈展开至派生成员析构前排空借用线程。

Application 回调签名、flight 数量、场景交付和 RHI Acquire/Present 未改变。独立窗口默认即时派发；
Cocoa 工厂明确拒绝延迟模式，未增加 Cocoa runtime。无窗口主机使用条件变量进度通知，线程尾声使用有界检查。
Win32 的 WM_QUIT 被记录为 WindowManager 退出条件，不伪造主窗口关闭通知。
正常验证配置与既有 Vulkan 多交换链版本门槛未改变；没有新增全局共享状态、异常控制流或生产测试观测字段。

## 初次验收覆盖与限制

| 约束 | 实际证据 |
|---|---|
| CPU 交付不阻塞窗口消息 | CpuHandoffWaitServicesSentMessages：RT SendMessageTimeout，覆盖 F=1 背压、维护与退出 |
| 真实 GPU gate | InputDuringGpuStall / SentMessageDuringGpuStall / WindowMutationWaitsForGpu：释放前不推进 Update、输入或修改 |
| 独立队列 idle | ShutdownDrainsQueueTailAfterFinalFlightFence：F=1 的最终 fence 已退休，再追加独立队列等待，关停仍等真实队列排空 |
| 输入完整性 | 单次 GPU 停顿内 27 条通知：键盘 down/repeat/up、文本及 UTF-16 代理对、鼠标 down/up、纵/横滚轮、焦点和捕获；逐条比对顺序及最终状态 |
| 冻结与身份 | NativeEventPumpTest：回调发送的新文本留到下一批、禁止嵌套派发、注销/销毁取消旧身份、300 条消息分批采集 |
| flight 与 owner | FlightCountAndOwnerRetirementMatrix：双后端、单/双线程、F=1/2/3/4；19 个发布帧按序且只完成一次，owner 真实完成后释放；保留 SlotReuseOverlapsTheNextRecording |
| 受限回收 | RetireUsesOnlySubmittedPrefixAndFinalSignalOrder：双后端，逆序保留两个身份后顺序提交，第三槽仍在录制；仅退休已交付范围 |
| 模态与停止 | CompletionInputBeforeUpdate 的原生 timer；GpuStallModalCloseAndMaintenanceStop 的进入/退出拖动、VBlank、保持消息栈时推进停止、外层才 OnShutdown |
| 真实菜单循环 | StopUnwindsNativeMenuBeforeOuterDestruction：真实 TrackPopupMenu 内请求停止，取消菜单后才返回外层；未依赖超时兜底 |
| 维护、窗口与取消 | WindowOperationsTest、既有 WindowLifecycle/WindowOperations、多窗口和最小化恢复回归；新增维护排空中关闭 |
| CPU-only / 初始退出 | ApplicationSystems、CpuOnlyTerminalEventsAndDeferredShutdown：无 GPU/无窗口、OnInit 模态 tick 不触发 Update、初始退出、真实关闭与 RequestExit、回调调用 Shutdown 仅请求退出 |
| 首帧计时 | FirstDeltaTimeExcludesInitialization：OnInit 故意耗时 50 ms，首帧 DeltaTime 不含这段耗时；先验证该测试在修正前失败 |
| 唤醒 | NativeEventPumpTest：提前/合并 Wake 与真实线程句柄退出；正常 RT、排空完成及跨线程 RequestExit 的集成路径 |

所有阻塞探针有失败超时，GPU helper 在非致命断言失败后仍释放 gate。真实 host-signaled gate 用例沿用既有
原生门控测试的无验证配置，以免测试专用信号绕过验证包装层的账本；普通正确性用例仍启用验证。
相对计划的验证限制：未注入分配异常以单独执行“异常绕过正常 Shutdown”的析构场景；析构的空/失败初始化路径及共用 Drain 路径有覆盖，
活动 runtime 的析构兜底通过同一路径和代码检查确认，不能把它描述为异常注入实测。未在 macOS 构建或运行 Cocoa。
同步 OnRender、后端/驱动阻塞、设备故障恢复与操作系统合并鼠标消息不在本次解决范围。

## 初次构建与测试记录

环境为 Windows、Visual Studio 18 2026 + ClangCL、CMake 4.4.2，使用既有多配置 `build_debug`，
GPU 为 NVIDIA GeForce RTX 4080。安装的 Vulkan SDK/验证层为 1.4.363.0；未降低已有 1.4.364 门槛。

以下均为**本次实际执行**，不是沿用旧历史报告：

| 阶段 | 结果 |
|---|---|
| 改动前 Debug 基线 | 49/49 通过，37.70 s |
| 仅基础接口，Application 尚未切换 | 50/50 通过，36.61 s |
| 首次切换 | 33 项中 4 项失败：旧测试要求输入回调中嵌套 Update；按已确认的禁止重入契约改为拒绝嵌套，并增加原生 timer 模态验证 |
| 更新后的 runtime 相关回归 | 35/35 通过，47.16 s；随后真实 GPU 停顿/关闭组合 2/2 通过，8.11 s |
| 阶段完整 Debug（最终三个用例加入前） | 768 项，762 通过、6 跳过、0 失败，494.33 s |
| 模态停止补强及完整输入后 Debug 相关回归 | 61/61 通过，62.48 s |
| Release 相关回归 | 61/61 通过，61.21 s |
| 受限回收修正后 Debug 专项 | 44/44 通过，57.00 s |
| 最后模态与初始化补强后 Debug 专项 | 45 项中 44 通过，菜单测试因 TPM_NONOTIFY 抑制模态通知而失败；修正测试参数后全部 6 项 NativeEventPumpTest 通过，0.17 s，其余 44 项不受该测试参数修改影响 |
| 完整 Release（首帧计时修正前） | 769 项，763 通过、6 跳过、0 失败，472.73 s |
| 首帧计时回归 | 修正前 1/1 失败，测得 DeltaTime 50.253 ms、允许上界 1.069 ms；将计时起点移至 Run 入口 |
| 最终 Debug 相关回归 | 65/65 通过，64.39 s；包含首帧计时、全部窗口事件、RuntimeFoundation、WindowOperationsTest 和受限回收 |
| 最终完整 Release | 770 项，764 通过、6 跳过、0 失败，472.84 s；包含首帧计时修正后的全部改动 |

完整 Debug 与两次完整 Release 的 6 项跳过相同：1 项 Vulkan 专属 NativeRenderScope 用例在 D3D12 参数下跳过；另 5 项为既有
Vulkan 多交换链验证门槛（4 个 RuntimeMultiWindow、1 个 RuntimeVulkanSwapChain），本次没有扩大 skip。

主要命令（均先构建、后串行测试）：

```powershell
cmake --build build_debug --config Debug --target test_radray_runtime --parallel 12
ctest --test-dir build_debug -C Debug -R '^(RuntimeFoundation|WindowOperationsTest|ApplicationSystems|FrameTimeline)\.' -j 1 --output-on-failure
cmake --build build_debug --config Debug --target test_radray_runtime test_radray_window test_radray_core test_inline_vector test_radray_render test_radray_shader_compiler --parallel 12
ctest --test-dir build_debug -C Debug -j 1 --output-on-failure
ctest --test-dir build_debug -C Debug -R '^(NativeEventPumpTest|RuntimeFoundation|ApplicationSystems|WindowOperationsTest)\.' -j 1 --output-on-failure
cmake --build build_debug --config Release --target example_framework_stress test_radray_runtime test_radray_window test_radray_core test_inline_vector test_radray_render test_radray_shader_compiler --parallel 12
ctest --test-dir build_debug -C Release -R '^(NativeEventPumpTest|RuntimeFoundation|ApplicationSystems|WindowOperationsTest)\.' -j 1 --output-on-failure
ctest --test-dir build_debug -C Debug -R 'RetireUsesOnlySubmittedPrefixAndFinalSignalOrder|^RuntimeFoundation\.|^NativeEventPumpTest\.' -j 1 --output-on-failure
ctest --test-dir build_debug -C Debug -R '^NativeEventPumpTest\.' -j 1 --output-on-failure
ctest --test-dir build_debug -C Debug -R '^(NativeEventPumpTest|RuntimeFoundation|ApplicationSystems|WindowOperationsTest)\.|RetireUsesOnlySubmittedPrefixAndFinalSignalOrder' -j 1 --output-on-failure
ctest --test-dir build_debug -C Release -j 1 --output-on-failure
```

最后补回了 RT 的既有 `RenderFrame` Tracy zone，并在模态退出、初始化防重入和受限回收顺序补强后重新构建 Debug/Release 的窗口与 runtime 测试、Release 压力场景。
菜单测试使用有通知的 TrackPopupMenu；[Win32 参数说明](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-trackpopupmenu)规定 TPM_NONOTIFY 不向 owner 窗口发送通知，不能用它验证框架的模态状态入口。
工作日志保存在未入库的 `build_debug/frame_loop_*.log`，不作为可随 checkout 取得的附件。

## 后续调整：移除 ModalVBlank

2026-10-16，用户要求先删除 Win32 ModalVBlank，验证现有 modal loop 是否满足需求。
此时 HEAD 仍为上述完整基线，帧循环重构仍在工作树，未把提交号当作这些修改的快照。
删除 ApplicationLoop 中的 VBlank 线程、隐藏消息窗口、DXGI factory/output 枚举及其启动/关停/等待分支，
同时删除 runtime 为该路径直接链接的 dxgi。原生窗口的模态定时器直接连接 TickModal，单/双线程统一使用。
这替代了原计划中“保留 VBlank 辅助线程、异步回收”的决定；维护与关停的 GPU 排空线程继续保留。

新增 NativeModalTimerRendersAndReturnsToOuterLoop，分别运行 D3D12/Vulkan × 单/双线程 × 移动/缩放/菜单，共 12 组。
测试经 WM_SYSCOMMAND 的 SC_MOVE / SC_SIZE 或 TrackPopupMenu 进入真实系统模态循环，
仅由原生模态定时器推进，每组至少成功录制并提交 6 个模态 clear 帧；没有直接调用 EventModalLoopTick。
移动/缩放循环用回车退出，之后恢复普通 Update；菜单内由 Update 请求退出，确认退出嵌套消息栈后才 OnShutdown。
同时核对帧身份不重复、完成恰好一次；5 秒失败兜底只由 fixture 持有，实际运行未触发。
普通 GPU 验证保持开启，没有新增 skip 或生产观测字段。

| 删除后的验证 | 结果 |
|---|---|
| Debug 新增真实模态测试 | 2/2 通过，9.05 s；含上述 12 组 |
| Debug 原有相关回归 | 65/65 通过，64.93 s；覆盖 GPU 停顿、输入、维护、关停、CPU-only、flight 和完成语义 |
| Release 全部相关回归 | 67/67 通过，73.24 s；包含新增的 12 组真实模态配置，无失败或跳过 |

执行命令（构建结束后串行测试）：

```powershell
$env:RADRAY_TEST_REQUIRED_BACKENDS = 'd3d12,vulkan'
cmake --build build_debug --config Debug --target test_radray_runtime test_radray_window --parallel 12
ctest --test-dir build_debug -C Debug -j1 -R 'NativeModalTimerRendersAndReturnsToOuterLoop' --output-on-failure --output-log build_debug/modal_timer_debug.log
ctest --test-dir build_debug -C Debug -j1 -R '^(NativeEventPumpTest|RuntimeFoundation|ApplicationSystems|WindowOperationsTest)\.|RetireUsesOnlySubmittedPrefixAndFinalSignalOrder' -E NativeModalTimerRendersAndReturnsToOuterLoop --output-on-failure --output-log build_debug/modal_timer_related_debug.log
cmake --build build_debug --config Release --target test_radray_runtime test_radray_window example_scene_sync --parallel 12
ctest --test-dir build_debug -C Release -j1 -R '^(NativeEventPumpTest|RuntimeFoundation|ApplicationSystems|WindowOperationsTest)\.|RetireUsesOnlySubmittedPrefixAndFinalSignalOrder' --output-on-failure --output-log build_debug/modal_timer_related_release.log
```

以上是功能验证，未测量真实鼠标持续拖动的帧间隔，也未做 VBlank 开/关的性能 A/B。
WM_TIMER 使用 10 ms 的最小周期且优先级低，不能据此承诺高刷新率下的拖动流畅度。
下面的 Tracy 数据来自删除前的首次验收，覆盖普通双线程循环，不是本次模态路径的性能证据。
Release 的 example_scene_sync 已重新构建，可用于人工体验。该后续调整未重跑全量测试，前述全量计数属于删除前。
文档检查重新执行：全量仍仅报告下文列出的 4 处旧 artifacts 引用；排除该目录后通过（469 files、33 docs、178 anchors），
git diff --check 和修改的头源/测试文件 clang-format dry-run/Werror 均通过。

## 初次验收性能证据（删除 ModalVBlank 前）

改动前先构建并保留 Release 的 `example_framework_stress_baseline.exe`；其 SHA-256 为
`8928054a8c0f1ab9cad918a71a2e46986ef8a2652cdbd8e557fcb7a5a9e90157`。
Tracy capture/csvexport 版本为 0.14.1 / 30997d5，与本地 client 匹配。每次只运行一个压力场景，
开启同一 GPU profiler 和 Tracy，关闭验证；双后端、双线程 F=2，每种配置各 5 个独立进程，预热 120 帧后运行 4 s。
取 GT/RT 都预热结束后的 0.5–3.5 s 窗口，比较帧间隔、S0 执行与 GT Update/RT RenderFrame 区间交叠。
进程 CPU 时间包含启动与关停，单独列出，不能等同于稳态帧 CPU 成本或把等待 zone 的墙钟时间当作 CPU 使用时间。

```powershell
example_framework_stress.exe --d3d12 --multithread --flights=2 --seconds=4 --warmup=120 --mode=world --objects=0 --workload=idle --window
example_framework_stress.exe --vulkan --multithread --flights=2 --seconds=4 --warmup=120 --mode=sync --objects=1000 --changes=100 --workload=move
tracy-capture.exe -a 127.0.0.1 -o capture.tracy -s 20 -f
tracy-csvexport.exe -u capture.tracy
tracy-csvexport.exe -m capture.tracy
```

两种场景都分别运行 D3D12 和 Vulkan，旧/新程序参数相同；上面列出各一个命令示例。最终程序 SHA-256 为
`66650e302932ab40957093e92ad2e98bde2c061adb3348c5b2a1a87648709a32`（包含首帧计时修正）。
40 次进程和 capture 均返回 0；每次运行的数据、二进制及 trace SHA-256 归档在
[性能数据](2026-10-16-frame-loop-refactor-performance.csv)。完整 trace、CPU CSV、进程日志与采集脚本保留于本机忽略目录
`build_debug/frame_loop_profile/`，未入库，不能只靠 checkout 重取这些原始 trace。

下表为每组 5 个独立进程的中位数；吞吐按 3 s 稳态窗口的 GT Update 次数计算，窗口场景使用 Immediate Present。

| 后端 / 场景 | GT updates/s 旧 → 新 | GT 间隔 p99 ms 旧 → 新 | GT 与 RT 录制重叠比例 % 旧 → 新 | 进程 CPU s 旧 → 新 |
|---|---|---|---|---|
| D3D12 / window idle | 1906.7 → 6779.7 | 1.5198 → 0.2893 | 46.49 → 99.19 | 2.3750 → 5.7500 |
| Vulkan / window idle | 1882.0 → 10153.7 | 1.4995 → 0.1934 | 16.44 → 98.32 | 1.1875 → 3.8438 |
| D3D12 / headless sync move | 23513.3 → 23172.3 | 0.0939 → 0.0946 | 57.96 → 54.28 | 5.4531 → 5.4688 |
| Vulkan / headless sync move | 23608.3 → 23553.3 | 0.1041 → 0.1031 | 38.18 → 22.52 | 3.8750 → 3.5625 |

窗口场景的旧 CPU 交付等待没有 RT 进度句柄，需要靠 1 ms 超时重新检查；新事件唤醒去除了这部分帧间空洞。
总 CPU 时间随无帧率上限的吞吐增长；按完整进程 CPU / 4 s workload 更新总数归一，D3D12 从 306.21 降至
213.82 µs/update，Vulkan 从 155.94 降至 93.47 µs/update。该口径仍包含启动/关停，不能当作稳态单帧 CPU 成本。
新的 3 s 窗口内，GT 进度等待 / GPU 等待 zone 的墙钟中位数分别为 D3D12 2.340 / 0.287 s、Vulkan 2.063 / 0.725 s，
与源码中的事件等待及有限 fence 休眠一致；它们不是忙等 CPU 时间。

headless sync 的 D3D12 吞吐中位数下降 1.5%，但旧 5 次范围为 22713–23710、新为 21959–23602 updates/s；
Vulkan 中位数下降 0.2%，旧范围 22509–23983、新为 22276–23743。当前短样本未显示超出进程间波动的稳定吞吐退化，
也不足以承诺等价的精确百分比。重叠比例仅表示微小 Update zone 与 RenderFrame 时间区间相交的次数，不能解释为 CPU 利用率；
Vulkan headless 的该比例下降，而吞吐和 p99 间隔未同步恶化，旧槽复用与后续录制重叠另有确定性回归验证。
S0 均值旧/新分别为 D3D12 window 0.521/0.940、sync 0.389/0.570，Vulkan window 0.510/0.387、sync 0.312/0.425 µs；
S0 函数体未变，调度和重叠已改变，这些不足 1 µs 的 Tracy zone 结果不作为独立执行成本基准。
短时 Tracy 场景属于诊断证据，不是整机帧率承诺或专用 Google Benchmark 统计。

## 文档检查

已同步[帧与 GPU](2026-10-21-runtime-frame-and-gpu-snapshot.md)、[Runtime 宿主](2026-10-21-runtime-render-framework-snapshot.md)、
[构建与测试](2026-10-21-runtime-build-test-snapshot.md)及[性能测量](2026-10-21-runtime-performance-snapshot.md)。
`git diff --check` 通过。`python tools/check_docs.py` 检出 4 处本次未修改、被 Git 忽略的旧 artifacts 引用：

- `artifacts/scene_sync_100k_20261006/verification.json` 和 `verify.py` 指向已移除的旧临时文档。
- `artifacts/transform_store_20260924/make_report.py` 和 `analysis.md` 指向已移除的旧临时文档。

这些历史产物保持原样；没有改检查器或把全量检查失败写成通过。补充执行以下仓库文档范围检查，明确排除旧 artifacts：

```powershell
python -c "import tools.check_docs as c; c.SKIP_DIRS.add('artifacts'); raise SystemExit(c.main())"
```

该范围检查通过：469 files、33 docs、178 anchors。它不替代上面失败的原始全量命令。
新增协调器及测试文件的 clang-format dry-run/Werror 检查通过；用户原有 app_render_context.h 修改保持原样。
