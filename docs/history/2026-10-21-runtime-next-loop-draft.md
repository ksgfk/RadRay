> - 适用: 查阅下一版底层帧循环的未定稿输入
> - 权威: 用户提供的草案附件；未定稿、未实现，不是批准的 API 或规则
> - 锚点: `docs/history/2026-10-21-runtime-rewrite-rationale.md`

# 下一版帧循环草案原文附件

归档日期：2026-10-21。输入来源：用户桌面 `a.md`。原始文件 SHA-256：`e354f37c0b6dcd262c4c0a7b42607b2f48a08dd3f77681d417a39faf0755e11d`。
比较基线：`751aca4e6a70203e6a62f9de67893b33463b94fc`。下方分隔线之后按原始字节保存原文；原文中的“未保存”等描述属于其写作当时。
旧 Application 并存策略已由本次整体移除决定取代；其他待确认项、异常策略与 RHI API 修改均未获实施批准。

---
# RadRay 帧循环、推进与终止 v5（整合稿）

范围是底层的帧循环、推进、失败与终止。高层框架接入暂缓，见 §19.3。这份稿只在对话里给出：没有写文件，没有入库，也没有运行测试。

标注约定：
- 未标注的条目是设计建议。
- 标"现状"的附了代码位置，均已核对。
- 标"推断"的表示我没找到原文依据。
- 标"待确认"的需要你拍板，正文先按我的推荐写。

相对 v4 的修订：
1. 账本把"acquire 已被消费"和"Present 已入队"分开记录。T3 只为没被消费的 acquire 补空批（§5.2、§7.3、§12.4、§13）。
2. scope 能否析构，改由 on_empty 的完成确认决定（§9.4、§9.5）。
3. 屏障在执行维护前，先退休它覆盖的帧（§8.3、§10）。新增 R18 和 D17。
4. 等待记录分成两类，各自规定唯一处理权。普通安全点和终止安全点共用一套交付规则（§9.1 到 §9.3）。
5. T3 拆成 T3a 到 T3d。join RT 不再依赖 GPU 模式（§13、§17）。
6. Host 输入拆成状态和事件 FIFO（§11 H3）。
7. 旧 Application 路径并存到集成为止；D14 收窄到新 scope 类型（待确认）。
8. §19.1 的两处更正：
   - InvalidateView 的 abort 保留，作为不变量检查；
   - application_loop.cpp 不再替换，改为与新循环并存。
9. 新增解耦验收标准（§19.3）。

v4 里"三条时序已闭合"的结论已撤回。v5 的这些时序请你复核。

## 0. 目标与非目标

必须保持：
1. 槽位只在 GPU 真正完成后复用。
2. GT 与 RT 可以重叠执行。
3. 有安全的窗口维护点。
4. GPU 正在使用的对象保持存活。
5. 取消、完成、失败三者分离。
6. 支持 modal loop、无 GPU、headless、standalone。
7. 满足 DXGI/WSI 对消息泵的要求。
8. 终止时资源安全释放，进程不退出。

非目标：
- 检测 GPU 挂起，交给 TDR。
- 会话结束时完整执行终止协议，这里只做尽力而为。
- Vulkan 呈现资源释放的规范级保证，见 D12。
- 高层框架接入：World/Actor/Component、WorldManager、SceneManager 与 GPU 场景、AssetManager、现有 WindowManager。

## 1. 决策

| 编号 | 决策 |
|---|---|
| D1 | Host 是进程主线程，只泵消息和执行 Host 请求。GT 在独立线程运行，只用阻塞式驱动。RT 可选；不开 RT 时，RT 阶段在 Publish 内联执行 |
| D2 | 退出是原因为 Exit 的终止，在下一个检查点生效。已 Open 但未 Publish 的帧直接放弃 |
| D3 | 输出丢弃只有两个来源：app 调用 DiscardOutput；目标本帧不可呈现（包括 acquire 超时） |
| D4 | F = FlightDataCount；Q = MaxUnsubmittedFrames，取值 [0, F-1]，默认 min(1, F-1) |
| D5 | 每个队列有自己的完成点。帧队列归 RT，copy 队列归 upload 服务 |
| D6 | 设备丢失按 L1 处理；不检测挂起；删除 GPU 看门狗 |
| D7 | 收到 QUERYENDSESSION 不终止；收到 ENDSESSION(TRUE) 时由 Host 请求终止，尽力而为 |
| D8 | 边界只捕获 radray::RuntimeException，它定义在新文件 exception.h 中，NullableAccessException 改为派生自它。其他异常一律按 L2 处理 |
| D9 | Run 返回 RunResult{Reason, optional<RuntimeFailure>}，并提供 RethrowIfFailed() |
| D10 | Exit 和 Failure 都调用 OnShutdown；所有关闭路径都是 noexcept |
| D11 | OnApply 总是执行；OnRenderOutput 只在输出未丢弃时执行 |
| D12 | Vulkan 不用 swapchainMaintenance1；呈现资源在 WaitIdle 之后销毁 |
| D13 | acquire 用有限超时 T_acq，默认 100 ms，可配置 |
| D14（待确认） | 新 scope 类型（暂名 StrictTaskScope）析构时必须已 drained，否则所有构建 abort。规则对新类型全局生效，测试侧提供 RAII 收束守卫。旧 TaskScope 是唯一豁免，只在旧 Application 路径使用，集成时迁移并删除 |
| D15 | 不做 D3D12 RemoveDevice 回调 |
| D16 | 高层接入暂缓。过渡期旧 Application 路径原样保留，新循环作为并行入口 |
| D17（待确认） | 参与者不得跨帧持有目标图像及其 view，只能经 RecordScope 在本帧取用。备选方案是在屏障前加 OnTargetsChanging 钩子 |

## 2. 规则

- R1 严格帧序：Open、Publish、录制、提交、退休、完成交付，全部按 FrameId 递增执行。
- R2 交接通道：跨线程状态只能经下列通道传递。新增通道时必须写明写者、读者和 happens-before 关系。
  - 计数：published、submitted。
  - 槽位数据：Packet、SubmissionRecord、RT 槽位数据。
  - 凭据：CompletionPoint、upload 完成消息。
  - Host：请求队列、事件 FIFO 与状态块；另外还有 GT inbox、TerminationState、scope drained 通知。
- R3 Q 闸：Open(n) 要求 n - submitted ≤ Q。
- R4 F 闸：Open(n) 要求帧 n-F 已经执行完 RetireFrame。先过 Q 闸，再过 F 闸。
- R5 单写者：
  - GT 状态只由 GT 修改。
  - RT 持久状态只由 RT 修改，到 T4c 经 join 转交 GT。
  - 槽位数据按 §5.4 的规则在两者之间交替持有。
- R6 完成凭据：
  - GPU 等待只引用 CompletionPoint。
  - CompletionPoint 只能由队列拥有者在原生提交成功后构造，并且只经 R2 的通道交接。
- R7 Host 只在两处等待 GT：H8 的主泵和会话结束时的嵌套泵。两处都同时在泵消息。
- R8 安全点（包括终止安全点）是唯一的交付点。RetireGT 只做内部退休，不交付。
- R9 取消、完成、失败三者分离。等待记录的唯一处理权见 §9.1。失败只经 TerminationState 报告。
- R10 退出精确：GT 观察到终止后不再 Publish；RT 观察到终止后不再开始 EndRecord。
- R11 失败分为 L0、L1、L2 三级，见 §12。
- R12 try/catch/throw 只出现在三处：InvokeExternal、GuardTask、RethrowIfFailed。
- R13 等待规则：
  - 等已提交的 GPU 工作时，用一次无限等待。
  - 等待对象归等待线程所有。
  - RT 不阻塞等 fence。
  - acquire 用 T_acq。
- R14 关闭路径全部 noexcept。
- R15 StrictTaskScope 只在 IsDrained 之后析构；GT 上不允许同步等待 scope。
- R16 退休只经 RetireFrame(k) 执行，顺序固定为：ReleaseSlot(k) → 帧号 ≤ k 的 GT Defer → OnRetireGT(k) → 准备交付。
- R17 EndRecord 遵守 §7.3 的 E1 到 E3。
- R18 即 D17。

## 3. 线程

| 线程 | 职责 | 允许的阻塞 | 禁止 |
|---|---|---|---|
| Host | 窗口与消息泵、Host 请求、事件与状态、会话消息 | MsgWaitForMultipleObjectsEx，同时等消息和 GT 线程句柄 | 运行 app 代码；不泵消息就等待 |
| GT | app 与参与者的 GT 半边、executor、安全点、退休、屏障、终止 | GT 唤醒事件、GPU 凭据、终止时 join | 同步等待 scope |
| RT | 录制、提交、acquire、present，持有帧队列 | park；acquire 最多等 T_acq | 阻塞等 fence |
| Upload | 持有 copy 队列，发布上传凭据 | 等自己的凭据和请求 | 访问 GT 状态 |
| Worker | 执行 CPU job | job 内部 | 访问 GT 状态 |

## 4. 配置

```cpp
struct RunConfig {
    uint32_t FlightDataCount;                       // F >= 1
    uint32_t MaxUnsubmittedFrames;                  // Q in [0, F-1], 默认 min(1, F-1)
    bool RenderThread{true};                        // false: RT 阶段在 Publish 内联
    std::chrono::milliseconds AcquireTimeout{100};  // T_acq > 0
    GpuMode Gpu;                                    // Device 或 None; None 时允许 RenderThread=true
};
```

配置校验失败时返回 Failure(Startup)。Q = 0 时 GT 与 RT 不重叠。Gpu=None 加 RT 这个组合保留，原因是它不需要设备就能测试 §5.4 的交接协议。

## 5. 帧模型

### 5.1 标识
- FrameId：从 0 开始递增。
- FrameScope：从 Open(n) 起有效，到 Publish(n) 或放弃为止。
- FrameRef：指向已发布帧的引用。
- RecordScope：RT 录制帧 m 时持有。
- 槽位号 s = k mod F。

### 5.2 槽位数据
- Packet(k)：GT 写，RT 读。包含：
  - 参与者 delta；
  - 目标列表与各目标本帧是否可呈现；
  - Discard 标志；
  - 需要等待的 upload 凭据。
- SubmissionRecord(k)：RT 逐项追加的账本，只记录事实，不记录合并状态。

```text
QueueSubmit{queue, CompletionPoint}             原生提交成功后立即追加
TargetAcquire{target, imageIndex, acquireSemaphore,
              consumer: None | Submitted(CompletionPoint) | Unknown,
              present:  NotAttempted | Queued | NotQueued | Unknown}
TargetStatus{target, NeedsRecreate}             L0
State: Complete | Partial
```

- RT 槽位数据包括：
  - command allocator 与 command buffer；
  - framebuffer/pass pins、per-frame 绑定；
  - RT Defer、profiler query；
  - 参与者 RT 半边的槽位数据。

### 5.3 计数与完成
- published：GT 在 Publish(k) 时以 release 写入 k+1。
- submitted：账本 Complete 后，RT 以 release 写入 k+1。Partial 的帧不发布 submitted。
- completed(k)：要求 completed(k-1) 成立，且账本里的所有凭据都已达到。没有 GPU 工作的帧，前序帧完成即完成。这个条件只在 GT 上计算。
- retired：GT 本地计数。

### 5.4 槽位所有权循环

```text
GT  Open(k)           要求 RetireFrame(k-F) 已完成; GT 写 Packet(k)
GT  Publish(k)        published = k+1 (release)  → Packet(k) 与 RT 槽位数据归 RT
RT  录制 k            读 Packet(k); 写 RT 槽位数据与账本
RT  EndRecord 完整    submitted = k+1 (release)  → 账本与 RT 槽位数据归 GT
GT  RetireFrame(k)    槽位可用于帧 k+F
```

Partial 的帧和录制后被放弃的帧，GT 在 T3a join RT 之后才读取它们的槽位，join 本身提供 happens-before。

### 5.5 证明（前提是严格帧序）
- P1，Packet：
  - GT 只在 Open(k) 到 Publish(k) 之间写它，RT 在 published > k 之后才读。
  - GT 下一次写是在 Open(k+F)，这要求 RetireFrame(k) 已完成，进而要求 submitted > k，说明 RT 已经读完。
- P2，RT 槽位数据与账本：
  - GT 在 submitted > k 之后才访问。
  - RT 下一次访问是在 BeginRecord(k+F)，它晚于 Open(k+F)，而 Open(k+F) 又晚于 RetireFrame(k)。
- P3，GPU：槽位 k 的资源只在 completed(k) 之后被帧 k+F 复用。
- P4，跨帧引用：见 §8.2；目标图像另见 R18。

## 6. GT 主循环

```text
for (;;) {
    RetireGT();                                   // §8.3, 非阻塞
    if (term.Requested()) break;                  // C1
    RunPendingBarrier();                          // §10
    if (!WaitCanOpen(n)) break;
    Open(n);
    if (n == 0) InvokeExternal(app.OnInit);
    SafePoint(n);                                 // §9.2
    if (term.Requested()) { Abandon(n); break; }  // C2
    DeliverWindowEvents(n);                       // 本批事件, 含 OnCloseRequested
    InvokeExternal(app.OnUpdate); 参与者 OnUpdateGT;
    if (term.Requested()) { Abandon(n); break; }  // C3
    Publish(n);
    ++n;
}
Terminate();                                      // §13

WaitCanOpen(n):
    while (n - submitted.load(acquire) > Q) {
        if (term.Requested()) return false;
        WaitGTWake();
    }
    for (k = retired; k + F <= n; ++k) {
        WaitCredentials(SubmissionRecord(k));     // 逐个一次无限等待, 结果按 §12.4
        RetireGT();
        if (term.Requested()) return false;
    }
    return true;
```

- GT 唤醒事件由以下情况触发：submitted 推进、RT 退出、终止请求、inbox 投递、scope drained 通知。
- 因为 Q ≤ F-1，只要通过了 Q 闸，帧 n-F 的凭据就一定已经存在。
- Abandon(n)：Packet(n) 不发布，绑定到帧 n 的 GT Defer 留到 T4d。

## 7. RT

### 7.1 循环

```text
for (;;) {
    if (term.Requested()) break;
    if (barrier.Pending() && m == published) { ParkInBarrier(); continue; }
    WaitRTWake();
    if (term.Requested()) break;
    if (published.load(acquire) <= m) continue;
    BeginRecord(m);
    OnApply(m);                                          // D11
    if (!Packet(m).Discard) {
        OnRender(m);
        for (target : 本帧可呈现的目标) {
            switch (target.AcquireNext(T_acq)) {
                Success    → 账本 += TargetAcquire{consumer=None, present=NotAttempted}; OnRenderOutput
                RetryLater → 该目标本帧不可呈现 (D3)
                Recreate   → TargetStatus NeedsRecreate (L0)
                其他       → §12.4
            }
        }
    }
    if (term.Requested()) { AbandonRecord(m); break; }   // 账本 Partial
    if (!EndRecord(m)) break;
    submitted.store(m + 1, release); WakeGT();
    ++m;
}
```

### 7.2 Acquire
- 现状：
  - Vulkan 把 TIMEOUT/NOT_READY 映射为 RetryLater（vulkan_impl.cpp:4352-4395）。
  - D3D12 带超时等待 latency 对象，HWND 不可呈现时直接返回 RetryLater（d3d12_impl.cpp:1241、1268、4252-4291）。
  - 旧调用方在 modal loop 中传 0，其余情况传无限（window_manager.cpp:76）。
- 新规则：一律使用 T_acq。acquire 不在 Host 上执行，modal 分支可以去掉。
- 依据：VUID-07783。
- 推断：acquire 成功后 semaphore 会在有限时间内被 signal；DXGI 等过 latency 对象之后，Present 不再阻塞。

### 7.3 EndRecord
- E1：批次按依赖顺序提交。每个等待都必须有凭据，凭据来自本帧账本或 Packet 中的 upload 凭据。上游没有提交，本批次也不提交。
- E1a：等待图像 X 的 acquire semaphore 的批次，是 X 的消费批次。
  - signal X 的 present wait semaphore 的批次，必须就是消费批次或依赖它。
  - 消费批次提交成功时，写 X.consumer = Submitted(cp)。
  - 提交返回 UNKNOWN 时，写 Unknown。
- E2：遇到第一个 L1 失败，就停止本帧后续所有 submit 和 Present。账本置为 Partial，调用 term.Fail，返回 false。
- E3：signal 批次已经提交，才 Present X。由 E1a 可知，此时 X.consumer 必为 Submitted。Present 的结果写入 X.present。
- Present 返回 L0 结果时，这一帧仍然可以是 Complete。EndRecord 内部不设检查点。

## 8. 退休与 Defer

### 8.1 A 类：槽位绑定引用
- 指 RT 槽位数据。它们经交接归 GT，在 ReleaseSlot 中清理；参与者通过 OnReleaseSlot(SlotRef) 清理自己的部分。
- 现状：
  - view 销毁时要求 framebuffer 的 Uses 为 0（vulkan_impl.cpp:1041-1059）；
  - Uses 只在 allocator Reset 清理 pins 时释放（3271-3287），GPU 空闲不会自动清；
  - 释放 pin 时会加 cache 的锁（1022-1030）。
- InvalidateView 的 abort 保留，作为 L2 不变量检查。§8.3 和 §10 的顺序保证它不会触发。

### 8.2 B 类：跨帧持久引用
GT 在帧 k 删除对象时，把删除 delta 写入 Packet(k)，并把对象的 Defer 绑定到帧 k。RT 在 OnApply(k) 中丢弃对它的引用。

Defer(k) 执行时，以下四点同时成立：
1. 帧号 ≤ k 的 GPU 工作已经结束。
2. 帧号 ≤ k 的槽位都已按顺序 ReleaseSlot。
3. OnApply(k) 已完成，并经 submitted > k 对 GT 可见。
4. 帧号 > k 的帧不再引用这个对象。

### 8.3 RetireFrame 与 RetireGT

```text
RetireFrame(k)   前提: submitted > k (acquire), completed(k)
  1. ReleaseSlot(k): allocator Reset → 参与者 OnReleaseSlot(k) → 槽位内 RT Defer → profiler resolve
  2. 帧号 ≤ k 的 GT Defer, 按登记顺序
  3. 参与者 OnRetireGT(k): 只允许释放类操作, 不允许帧 API、写 Packet、Spawn、RequestExit
  4. 生成 CompletionEvent(k), 清空账本; GpuDone(k) 记录 Pending → Ready, 不恢复

RetireGT(): 从 retired 起, 逐帧非阻塞查询 completed(k); 成立就 RetireFrame(k), 否则停止
```

- 查询返回 DeviceLost（包括 D3D12 读到 UINT64_MAX）时，停止退休并调用 Fail，不把它当作完成。
- 推断：allocator 可以在 GT 上 Reset。
- 与 RT 并发访问的共享结构：render pass cache 已确认加锁；descriptor 分配器、profiler GPU context、设备级销毁路径还没有核对。

### 8.4 Defer 接口
- Defer(obj)：绑定到当前 FrameScope 所在的帧；终止期间登记的进入 T4d 列表。
- DeferUntil(obj, CompletionPoint)：用于 upload 使用过的对象。
- RT Defer：追加到当前槽位，在 ReleaseSlot 中执行。

### 8.5 删除纹理的端到端时序

```text
GT                               RT                             GPU
Update(k): Packet(k) += Remove(T); Defer(T, k)
Publish(k) ────────────────────▶ OnApply(k): 丢弃 T 的持久引用
                                 提交 k; submitted = k+1
                                                                完成帧号 ≤ k 的工作
RetireFrame(k): ReleaseSlot → pins 清零 → Defer 销毁 T
```

## 9. Executor、安全点与 scope

### 9.1 等待记录

```text
帧推进类: NextFrame, GpuDone(FrameRef), Barrier。无副作用, 生产者是 GT
  Pending → Ready              生产者 CAS
  Pending | Ready → Canceled   stop callback CAS; 不在 GT 上时随后唤醒 GT
  Ready → Delivered            GT 交付前 CAS 成功则以成功恢复, 失败则按 Canceled 处理
  Canceled → Delivered         GT 以 stopped 恢复

跨线程类: Host 回复, worker, upload。操作可能已产生副作用
  归属: 操作自身的 CAS, Queued → Taken (执行方) 或 Queued → Abandoned (取消方)
  记录: Pending → Ready(成功 | 失败 | stopped), 只由归属方写一次并投递 inbox
  stop callback 只转发取消, 不写记录; 不存在 Ready → Canceled
  Ready → Delivered            GT 按实际结果恢复
```

- 每条记录恰好恢复一次，GT 是唯一的恢复者。
- 跨线程操作的执行方必须在收到取消后，有限时间内写出记录：
  - worker 上还没开始的 job，直接 stopped；
  - Host 请求由 CAS 决定；
  - upload 以 stopped 或实际结果完成。
- GT 调度器上 ResumeOnCancel 固定为 false。

### 9.2 SafePoint(n)

```text
① 取走 Host 事件批与对应状态 (H3)
② 按 FrameId 交付 OnFrameCompleted
③ 冻结 sequence < boundary 的一批记录, 按 §9.1 逐条交付; 恢复中新建的记录留到下一个安全点
④ 析构 IsDrained 的 retiring scope
② ③ 每交付一项前读 term.Requested(): 为真则中止本次安全点, 由 C2 转入终止
```

- "已经观察到失败"的定义：失败记录 happens-before GT 的这次检查。此后不再交付帧推进类的成功；跨线程类照常按实际结果交付。
- 已知限制：Vulkan 在出现 DEVICE_LOST 之前，无法区分"已完成"和"设备已丢失"。依赖回读数据正确性的地方，以 RunResult 为准。

### 9.3 终止安全点
- 在 T1 中反复执行，只做 ③ 和 ④。T1 对所有 scope 调用 RequestStop 之后，帧推进类记录全部转为 Canceled。
- 终止期间续体能做什么：
  - 没有 FrameScope，帧 API 一律拒绝；
  - Defer 进入 T4d 列表；
  - 新建的帧推进类等待，直接以 Canceled 创建；
  - 新的跨线程操作因 stop token 已请求停止，不发起，直接 Ready(stopped)；
  - OwnedTasks.Spawn 销毁未启动的 task。
- 收束性：stopped 会展开协程，task 无法忽略。能拖住 T1 的，只有不响应取消的跨线程操作，见 §20。

### 9.4 StrictTaskScope（core，暂名）

```cpp
class StrictTaskScope {
public:
    ~StrictTaskScope() noexcept;                       // 未 Close 先 Close; 仍未 drained 则 RADRAY_ABORT
    void Spawn(task<void> t);                          // Close 之后调用为 L2
    bool RequestStop() noexcept;
    void Close(Nullable<DrainNotifier*> n) noexcept;   // 启动成员 on_empty 操作; 已空则就地 drained
    bool IsDrained() const noexcept;                   // acquire 读 _drained
    task<void> Join();
    void WaitUntilEmpty() noexcept;                    // 显式阻塞; GT 上禁止, 由 runtime 层保证
    stop_token GetStopToken() const noexcept;
};
```

- on_empty 的 receiver 按顺序做三件事：
  1. 把 notifier 拷到局部变量；
  2. 以 release 写 `_drained = true`；
  3. 经局部变量通知。

  写完标志后不再访问自身。notifier 的生命周期必须长于 scope。
- 依据（现状）：
  - spawn 的下游完成后，`__complete` 仍要用 scope 指针加锁（async_scope.hpp:163-209）；
  - waiter 由最后一个 `__complete` 在锁内取出，解锁后才通知（169-178）；
  - scope 已经为空时，on_empty 在锁内确认后就地完成（88-103）；
  - `__impl` 析构时断言计数为 0、waiter 为空（62-67），只用 assert 检查（__config.hpp:852-864）。
- Spawn 时包装出的计数只用于错误消息，注明可能滞后，不作为析构依据。

### 9.5 OwnedTasks（runtime）
- 组成：一个活跃 StrictTaskScope 加一个 retiring 列表，向 GT executor 登记。
- 接口：
  - Spawn：只在 GT 调用，终止后改为丢弃。
  - Restart：Retire 当前活跃 scope，再新建一个。
  - Retire：Close(GT notifier) + RequestStop + 移入 retiring。
  - IsDrained：活跃 scope 已 Close 并 drained，且 retiring 为空。
- S4 契约：owner 必须活到它所有 retiring scope 都析构之后。高层怎么满足这条，随高层接入暂缓。
- executor 的逻辑任务计数只用于 T1 慢收束诊断。

### 9.6 owner 替换自己的 scope

```text
GT 帧 n    Restart: 旧 scope Close + RequestStop → 帧推进类记录 Canceled; 移入 retiring
GT 帧 n+1  SafePoint ③  任务以 stopped 恢复并结束
           最后一个完成者 (可能在 worker) 触发 on_empty: _drained = true, 唤醒 GT
           SafePoint ④  IsDrained, 析构旧 scope
```

### 9.7 任务错误

```cpp
task<void> GuardTask(TerminationState& term, task<void> t) {
    try {
        co_await std::move(t);
    } catch (const RuntimeException&) {
        term.Fail(RuntimeFailure::FromCurrent(FailureKind::Task));
    }
}
```

stopped 会直接穿过这个包装。其他异常会到达 set_error，导致进程终止（async_scope.hpp:759-764），属于 L2。

## 10. 屏障（窗口维护点）

```text
RunPendingBarrier (Open(n) 之前, 有请求时):
  1. 设屏障请求; RT 录完 published 之前的帧后 park 并确认
     此时 submitted == published; 否则 RT 已因失败退出, GT 转入终止
  2. 等待 [retired, submitted) 的全部凭据, 用 GT 的等待对象, 可含 copy 凭据
  3. 对 [retired, submitted) 按帧序 RetireFrame(k); 通知留到下一个安全点 (R8)
  4. 需要销毁 swapchain 时, 对帧队列 vkQueueWaitIdle (D12); RT 已 park, GT 临时负责外部同步
     不用 DeviceWaitIdle: upload 线程可能仍在提交
  5. 执行维护: 尺寸为 0 时不重建, 目标保持不可呈现; 重建失败按 L1 GpuError
  6. 释放 RT
屏障期间出现终止: GT 转入终止; RT 在 park 中看到终止后退出
```

- 第 3 步之后所有槽位都已释放，pins 清零。R18 保证槽位之外不残留对目标图像的引用。
- 推断：D3D12 的 ResizeBuffers 也要求先释放 back buffer 引用，同一顺序覆盖。

## 11. Host

H1：Host 是进程主线程，创建并持有所有 HWND，不运行 app 代码。

H2：请求队列，用 message-only 窗口唤醒 Host。

```text
请求状态: Queued → Taken → Done, 或 Queued → Abandoned

GT 端 Post(r):
  lock
    if broken: unlock; r.Complete(HostUnavailable); return
    queue.push(r); post = !wakePending; wakePending = true
  unlock
  if post && !PostMessageW(msgHwnd, WM_RADRAY_WAKE):
    lock; wakePending = false; broken = true; batch = take_all(); unlock
    for r in batch: if CAS(Queued, Abandoned): r.Complete(HostUnavailable)
    term.Fail(HostChannel)

GT 端取消 r: CAS(Queued, Abandoned) 成功则立即完成; 失败说明 Host 已取走, 等它回复

Host 收到 WM_RADRAY_WAKE:
  lock; wakePending = false; batch = take_all(); unlock
  for r in batch: if CAS(Queued, Taken): 执行请求, 回复投递到 GT inbox
```

H3：输入分为两部分，在同一把锁下发布。
- 状态，新值覆盖旧值：尺寸、最小化、遮挡、DPI、当前焦点。
- 事件，按 FIFO 保存，带递增序号，不额外合并：
  - 按键与鼠标按钮边沿、重复键；
  - UTF-8 文本（由队列拥有）、滚轮；
  - 焦点变化、捕获丢失；
  - CloseRequested、会话 info 事件。
- GT 在 ① 一次性取走截至当前序号的全部事件，以及与之对应的状态；回调期间新到的事件留到下一个安全点。
- 窗口销毁时，取消该窗口积压的事件。
- 系统已经合并的鼠标移动不恢复。这与现状契约一致（frame-and-gpu.md:269-273）。
- T0 之后不再执行 ①，积压事件随之丢弃。

H4：消息映射。
- WM_CLOSE → FIFO 中的 CloseRequested，GT 据此调用 OnCloseRequested。
- WM_QUERYENDSESSION → 立即返回 TRUE，不终止。
- WM_ENDSESSION(FALSE) → 不终止。
- WM_ENDSESSION(TRUE) → 见 H8。

H5：唤醒消息投递给窗口，modal loop 也会分发它。已核对文档：PostThreadMessage 在 modal loop 中会丢失。

H6：DXGI 要求窗口线程持续泵消息，由 R7 保证。

H7：窗口操作以 Host 请求执行，属于 §9.1 的跨线程类记录。

H8：退出与会话结束。
- 主泵同时等消息和 GT 线程句柄。GT 退出后，Host 依次：把剩余请求置为 Abandoned → 销毁剩余窗口 → 返回 RunResult。T6 发出的销毁请求只是优化，正确性不依赖它。
- 会话结束：
  - 第一个收到 ENDSESSION(TRUE) 的窗口调用 RequestSessionEnd，然后嵌套泵消息直到 GT 退出；
  - 嵌套泵期间照常处理请求，但不销毁窗口；
  - 其他窗口收到这条消息时直接返回。
- 承诺：正常关闭保证完整执行 T0 到 T6；会话结束只尽力而为，OnShutdown 不保证执行。

## 12. 失败

### 12.1 分级
- L0：本地恢复，例如目标重建。acquire 超时不算失败。
- L1：触发终止，原因为 Failure，进程存活。
- L2：程序错误、不变量被破坏、非 RuntimeException 的异常。进程 abort。

### 12.2 FailureKind
Startup、External、Task、DeviceLost、GpuError、HostChannel。

### 12.3 TerminationState

```cpp
class TerminationState {
public:
    bool Requested() const noexcept;          // acquire 读
    void RequestExit() noexcept;              // 只在 GT 调用
    void RequestSessionEnd() noexcept;        // 只在 Host 调用
    void Fail(RuntimeFailure f) noexcept;     // 任意线程
};

template <class F>
void InvokeExternal(TerminationState& term, F&& f) noexcept {
    try {
        std::forward<F>(f)();
    } catch (const RuntimeException&) {
        term.Fail(RuntimeFailure::FromCurrent(FailureKind::External));
    }
}
```

- 第一个请求决定 Reason。第一个失败是主失败，之后的失败作为次级失败附在后面，列表有上限且容量预先分配。
- 先 Exit 后失败时，Reason 保持 Exit，RunResult 带上该失败。
- 每次请求都会唤醒 GT 和 RT。实例归运行时所有，不是全局状态。
- RethrowIfFailed：保存了原始异常就重新抛出它，否则抛出 RuntimeException。

### 12.4 RHI 结果分类

| 调用 | 结果 | 处理 |
|---|---|---|
| Submit | 成功 | 构造 CompletionPoint 入账；若是消费批次，写 consumer = Submitted |
| Submit | DEVICE_LOST、设备移除 | L1 DeviceLost |
| Submit | OOM | L1 GpuError；视为未提交，consumer 保持 None（规范保证状态不变） |
| Submit | UNKNOWN | L1 GpuError；消费批次写 consumer = Unknown（推断：提交状态未知） |
| 任意 | VALIDATION_FAILED | L2 |
| Present | SUCCESS、SUBOPTIMAL | present = Queued |
| Present | OUT_OF_DATE、SURFACE_LOST、FSE 丢失 | present = Queued；L0 重建 |
| Present | OOM | present = NotQueued；L1 GpuError；同步对象状态不变 |
| Present | UNKNOWN | present = Unknown；L1 GpuError |
| Present | DEVICE_LOST | present = Unknown；L1 DeviceLost |
| Acquire | TIMEOUT、NOT_READY | 目标本帧不可呈现 |
| Acquire | OUT_OF_DATE、SURFACE_LOST | L0 重建 |
| Acquire | DEVICE_LOST、OOM | L1 |
| 运行期等待或查询 | DeviceLost、OOM | L1 DeviceLost、L1 GpuError |
| D3D12 完成值查询 | UINT64_MAX | DeviceLost |
| T3b 等待 | Completed、DeviceLost | 视为 GPU 不再访问 |
| T3b 等待 | 其他错误 | L2 |
| T3c 空批提交或等待 | 非丢失错误 | L2 |

D3D12 的具体 HRESULT 在实施时逐个核对。

## 13. 终止

```text
T0  请求生效 (第一个请求获胜), 唤醒 GT 与 RT
    GT 在 C1/C2/C3 退出循环, 已 Open 的帧放弃; RT 在帧起点或 EndRecord 前退出, 已录制未提交的帧放弃
T1  所有 OwnedTasks: Retire 活跃 scope; Spawn 转为丢弃
    反复执行终止安全点 (§9.3), 直到所有 OwnedTasks IsDrained
T2  停止 upload 并 join, 收集其凭据; join worker
T3a join RT (只要有 RT, 与 GPU 模式无关); 读取全部槽位账本, 包括 Partial
T3b 对 QueueSubmit 与 upload 凭据逐个无限等待                    (无 GPU 跳过)
T3c 对 consumer == None 的 TargetAcquire, 提交只等待其 acquire semaphore 的空批并等待完成
    设备已丢失时跳过; consumer 为 Submitted 或 Unknown 时不补    (仅 Vulkan 且有窗口目标)
T3d vkDeviceWaitIdle: 所有队列拥有者都已 join; 覆盖呈现资源 (D12)  (仅 Vulkan)
T4a 对所有槽位 ReleaseSlot, 不生成完成事件
T4b OnShutdown: app 仅在 OnInit 调用过时; 参与者全部
T4c 参与者 DestroyRTState, 在 GT 上执行
T4d 执行所有剩余 GT Defer, 包括终止期间登记的
T5  按创建逆序拆除: 参与者、服务、目标与 swapchain、设备级对象、设备
T6  向 Host 发送销毁窗口请求 (不等待回复), 写入 RunResult, GT 退出
```

- T1 必须早于 T2，因为任务可能还在等 upload 的结果。
- T4c 必须早于 T4d：被放弃帧的 Packet 从未到达 RT，RT 持久状态里可能还引用着这一帧删除的对象。
- 终止期间发生的失败记为次级失败。只有 T3b 和 T3c 的非丢失错误会 abort。
- 推断：Present OOM 后，present wait semaphore 保持已 signal，在 T5 销毁是合法的。

部分提交失败时的时序：

```text
RT EndRecord(k)
  提交 A (graphics, 等待 acquire(X), 写 X) → 成功: 账本 += A@a; X.consumer = Submitted(A@a)
  提交 B (compute, 等待 A@a)               → DEVICE_LOST 或 OOM
    term.Fail; 账本 Partial; 跳过 Present(X), X.present = NotAttempted
    不发布 submitted; RT 退出
GT  若停在 Q 闸, 被唤醒 → T0
T3a join RT
T3b 等待 A@a
T3c X.consumer 非 None, 不补空批
T3d vkDeviceWaitIdle
T4a ReleaseSlot(k)
```

变体：如果是 A 提交 OOM，那么 X.consumer 仍为 None，B 不会提交。设备未丢失时，T3c 为 X 补一个空批。

## 14. 启动

```text
Host 主线程, Run(app, config):
  校验 config, 失败返回 Failure(Startup)
  创建 message-only 窗口和初始窗口; 启动 GT; 进入主泵 (H8)
GT:
  创建 TerminationState 与 executor
  创建设备与帧队列; 启动 upload 与 worker; 创建目标与 swapchain; 启动 RT
  帧 0: Open → OnInit → SafePoint → OnUpdate → Publish
启动失败: Fail(Startup), 逆序撤销已完成的步骤; OnInit 未调用, 不调用 app 的 OnShutdown
```

## 15. App 接口

```cpp
class App {
public:
    virtual ~App() noexcept = default;
    // GT
    virtual void OnInit(FrameScope&) {}
    virtual void OnUpdate(FrameScope&) {}
    virtual void OnCloseRequested(FrameScope& frame, WindowId) { frame.RequestExit(); }
    virtual void OnFrameCompleted(const CompletionEvent&) {}
    virtual void OnShutdown(const ShutdownInfo&) noexcept {}
    // RT
    virtual void OnApply(RecordScope&, const Packet&) {}
    virtual void OnRender(RecordScope&) {}
    virtual void OnRenderOutput(RecordScope&, OutputTarget&) {}
};

struct ShutdownInfo {
    TerminationReason Reason;
    bool SessionEnd{false};
    Nullable<const RuntimeFailure*> Failure;
};
```

- FrameScope 提供：
  - FrameId、当前状态、本批事件（只在本帧有效）；
  - RequestExit、DiscardOutput、Defer；
  - 写 Packet、当前帧的 FrameRef。
- CompletionEvent 包含：FrameId、是否丢弃了输出、每个目标是否已呈现（present == Queued）。
- GpuDone 在帧退休后的那个安全点恢复。

## 16. 参与者接口

```cpp
class FrameParticipant {
public:
    virtual ~FrameParticipant() noexcept = default;
    // GT
    virtual void OnUpdateGT(FrameScope&) {}
    virtual void OnReleaseSlot(SlotRef) noexcept {}
    virtual void OnRetireGT(FrameId) {}
    virtual void OnShutdown(const ShutdownInfo&) noexcept {}
    // RT
    virtual void OnApply(RecordScope&, const Packet&) {}
    virtual void OnRender(RecordScope&) {}
    virtual void OnRenderOutput(RecordScope&, OutputTarget&) {}
    virtual void DestroyRTState() noexcept {}   // T4c, 运行在 GT 上
};
```

所有非 noexcept 的钩子都经过 InvokeExternal 调用。RT 半边遵守 R18，目标图像和 view 只经 RecordScope 在本帧取用。

## 17. 运行模式

| 模式 | 与标准模式的差异 |
|---|---|
| 标准 | Host、GT、RT、Upload、Worker 全部启用 |
| 单线程渲染 | 没有 RT 线程，RT 阶段在 Publish 中内联；Q 闸总是通过；T3a 无需 join |
| Headless | 没有窗口和 WSI，只有离屏目标；跳过 T3c |
| 无 GPU | 没有设备；跳过 T3b 到 T3d；T3a 照常执行；允许启用 RT |
| Standalone | 没有 Host 线程，调用方线程作为 GT；没有窗口；可以有 RT，T3a 照常执行 |

## 18. 测试

core（`modules/core/tests/`）：
- StrictTaskScope：
  - 已空时 Close 立即得到 drained；
  - 任务在 worker 完成时，只有收到 on_empty 通知后 IsDrained 才为真；
  - 加一组压力用例：worker 完成后 GT 立刻析构，反复运行。
- death test：析构时未 drained、Close 之后再 Spawn。
- RequestStop 后被取消的任务全部完成，scope 随之 drained。
- 测试侧提供 RAII 收束守卫。旧 TaskScope 的 4 个调度器用例不变。

render（`modules/render/tests/`）：
- Submit 注入 OOM：不构造 CompletionPoint，Vulkan 的 fence 值不前进（对应 R6，修正 3175-3186）。
- Wait、WaitFor、查询在设备丢失时返回 DeviceLost，不再 abort。`test_d3d12_device_loss.cpp` 的 6 个参数改为普通用例：断言返回 DeviceLost，进程存活。
- AcquireNext 在有限超时下返回 RetryLater。没有显示环境时跳过。
- Vulkan：带缓存 view 的纹理，在 allocator Reset 之后删除不 abort。这条是 R16 顺序的回归用例。

runtime（`modules/runtime/tests/`），fixture 自己持有 probe，不在生产代码加字段：
- 交接协议：用 Gpu=None 加 RT 的组合，验证以下几点：
  - R1 帧序、Q 闸、F 闸；
  - probe 记录访问线程与时序，在压力下检验 P1 和 P2；
  - 终止时 T3a 先 join RT，T4 才访问槽位。
- Defer：对象在 RetireFrame(k) 的第 2 步才销毁，且晚于 OnApply(k)。
- GpuDone：在 RetireFrame 之后的那个安全点恢复，不在 RetireGT 内恢复。
- 记录交付：
  - GpuDone 处于 Ready 时 Restart，任务以 stopped 恢复；
  - Host 请求已被取走后再取消，交付实际结果；
  - 请求被 Abandoned 时交付 stopped；
  - 每条记录恰好恢复一次。
- 终止中的记录：
  - GpuDone 处于 Ready 时终止，T1 能收束；
  - 终止后 Spawn 的 task 被丢弃；
  - 终止后新建的等待直接取消；
  - 续体中的 Defer 进入 T4d。
- 屏障：GPU 在屏障等待期间完成，ReleaseSlot 早于维护执行。在 Vulkan 真实设备上重建 swapchain，InvalidateView 不 abort。
- 事件：
  - 两个安全点之间先按下再松开，两条事件都按序交付；
  - CloseRequested 不会被后来的状态覆盖；
  - 窗口销毁会取消它积压的事件。
- 部分提交：按 §13 的时序。T3b 只等 A@a，T3c 不补空批，T4a 释放该槽位。
- 消费批次 Submit OOM：consumer 为 None，T3c 补一个空批。在这条上注入空批错误，用 death test 验证 L2。
- Present OOM：consumer 为 Submitted，T3c 不补空批，验证层没有 semaphore 报错。
- acquire 超时并关窗：终止在有限时间内走完 T0 到 T6。
- 在每个检查点注入终止：SafePoint 的四个阶段、屏障中、RT 的 EndRecord 前、Publish 前后，分别注入 RequestExit 和 Fail。验证：
  - 已打开的帧被放弃（D2）；
  - T4a 释放全部槽位；
  - OnShutdown 恰好调用一次，且只在 OnInit 已调用时调用（D10）；
  - RunResult 正确。
- 异常：RuntimeException 得到 Failure(Task/External)，RethrowIfFailed 能重新抛出。其他异常用 death test 验证。
- Host 队列：注入一个假 poster，N 个线程并发 Post，poster 恰好被调用一次，之后能再次唤醒；poster 失败时得到 Failure(HostChannel)。
- 会话消息：
  - QUERYENDSESSION 和 ENDSESSION(FALSE) 都不进入终止；
  - ENDSESSION(TRUE) 后结果为 Exit，SessionEnd 为 true；
  - T6 不等待回复。

失败注入：
- runtime 允许外部传入 Device。
- 测试侧写装饰器，包装 Device 及它创建的 Queue、SwapChain、Fence，按计划表在第 N 次 Submit、Present、Acquire、Wait 时返回指定结果。
- OOM 类结果跳过真实调用，使后端状态与规范语义一致。
- 先例：test_rhi_validation.cpp:272-284 的 ProbeSwapchain。

## 19. 影响范围

### 19.1 文件级改动

core：
- `coroutine.h`：
  - 新增 StrictTaskScope，非内联实现放进对应的 .cpp；
  - 旧 TaskScope（207-242）和旧调度器的 inline 取消（43、109-121、158-166）不动。
- 新增 `exception.h/.cpp`：RuntimeException。现状 modules 下没有这个类型。
- `test_manual_coroutine_scheduler.cpp`：不动，新类型的用例单独新增。

render：
- `rhi.h`：Submit、Present、Acquire、Wait 改为返回结果类；AcquireNext 去掉默认的 UINT64_MAX（1510）。
- `d3d12_impl.cpp`：
  - check 和 fence 等待中的 abort（55-70、3360-3365、3390-3394）改为返回结果；
  - `_event`（3341）改为等待方自带事件；
  - AcquireNext 超时归入不可呈现。
- `vulkan_impl.cpp`：
  - 提交成功后才签发凭据（3175-3186）；
  - Wait 的 abort（4156-4158）改为返回结果；
  - AcquireNext 加入超时处理；
  - 销毁 swapchain 前执行 WaitIdle；
  - InvalidateView 的 abort 保留。
- 旧路径的调用方要适配新的结果类型，失败时保持现状行为（RADRAY_ABORT）。
- `test_d3d12_device_loss.cpp`：契约翻转，见 §18。

runtime：
- 新增 Host、GT 驱动、RT、RunResult、OwnedTasks 等文件，命名待定，遵守 `.h`/`.cpp` 对应规则。
- `application.cpp`、`application_loop.cpp`、`gpu_system.cpp`、`window_manager.cpp` 原样并存到集成（D16）。现状：旧 Application 负责创建 WindowManager、SceneManager、WorldManager、AssetManager（application.cpp:317-395）。
- 旧路径继续传无限的 acquire 超时，VUID-07783 的风险保留到集成。

测试：新增 §18 的用例。test_asset_slot.cpp 和现有 27 处 TaskScope 声明都不动。

文档：实施时为新循环新增所属文档；frame-and-gpu.md 中旧路径的章节保留到集成。AGENTS.md 的增补（try/catch 位置、D14、R18）经你同意再写。docs/history 不写。

### 19.2 过渡期隔离（待确认）
- 两种 scope 并存：
  - 旧 TaskScope 保持现有语义，析构时 RequestStop + WaitUntilEmpty，配合旧调度器的 inline 取消，只在旧 Application 路径使用；
  - StrictTaskScope 只在新循环使用。
- 两种类型不能跨路径传递。这一点无法静态强制，只能靠 review。
- 代价：过渡期 D14 对旧代码和旧测试不成立；集成时要做一次迁移和改名。
- 不选 (a) 的原因：旧循环没有 SafePoint ④，组件移交出去的 scope 在旧路径上找不到排空点。

### 19.3 暂缓的集成项与验收
- 暂缓项：
  - World/Actor/Component 生命周期（world.cpp:126-220）及 draining 设计；
  - WorldManager；
  - SceneManager 与 GPU 场景；
  - AssetManager：keep-alive、`_loadScope` 收束、析构顺序（asset_manager.cpp:176-211 现状依赖 Application::Shutdown 先 wait-idle）；
  - WindowManager 与 Host 的关系；
  - 组件的 scope 迁移（static_mesh_component.cpp:17、26、55、56，instanced_static_mesh_component.cpp:40、48、183）。
- 验收标准：
  - 新增一个 system 不需要修改 loop；
  - system 可以脱离 Host 和 RT 独立驱动。

  第二条现在就可以用测试夹具直接按协议调用 FrameParticipant 的钩子来验证。

## 20. 风险与待定

风险：
- 不协作的 CPU 任务，或不响应取消的跨线程操作，会拖住 T1。缓解：超过阈值时打印未完成的 scope 和记录，不强杀。
- Vulkan 在观察到设备丢失之前，可能先报 GpuDone（推断）。缓解：L1 之后不再信任任何完成点。
- D12 没有规范保证：vkQueueWaitIdle 不覆盖 present。另外 resize 时多一次停顿。你已接受这个代价。
- 没有 RemoveDevice 回调：空闲或最小化期间，发现设备移除会延迟。
- 会话结束只是尽力而为，OnShutdown 不能承担持久化。
- 测试中 ASSERT 提前返回时，未 drained 的 scope 会 abort 整个测试进程。缓解：收束守卫声明在 scope 之后，让它先析构。
- 模态循环中 PostMessage 失败：失败会被记为 HostChannel，但要到模态循环结束后才能被观察到。
- GT 长时间停在 Q 闸或 F 闸时，事件 FIFO 会持续增长，上界大致是 TDR 时长（推断）。
- 两种 scope 类型并存，混用只能靠 review 发现。
- on_empty 的 receiver 如果写完标志后还访问自身，就是 UAF。这需要压力测试覆盖。
- 旧路径的无限 acquire 保留到集成。

待定：
- P1 不可呈现期间的帧节奏（推断）：D3D12 最小化时立即返回，Vulkan 尺寸为 0 时不 acquire，循环会空转。
- P2 失败注入 API 的形状。
- P3 外部窗口宿主模式。
- P4 D14 收窄与 19.2 的隔离方案。
- P5 D17 用约束还是钩子。
- P6 StrictTaskScope 的正式名称。

## 附录

A. 已核实

代码：
- `coroutine.h:43、109-121、158-166、207-242`
- `async_scope.hpp:62-67、88-103、163-209、759-764、813-881、842-851`
- `__config.hpp:852-864`
- `vulkan_impl.cpp:1022-1030、1041-1059、3150-3189、3271-3287、4148、4156-4158、4335-4409`
- `d3d12_impl.cpp:55-70、1241、1268、3341、3360-3365、3390-3394、4252-4291`
- `rhi.h:1494、1510`
- `window_manager.cpp:76、588-622`
- `gpu_system.cpp:153-176`
- `application_loop.cpp:296-306、320-321`
- `application.cpp:317-395`
- `asset_manager.cpp:165-211`
- `static_mesh_component.cpp:1-90`
- `test_asset_slot.cpp:960-990`
- `test_d3d12_device_loss.cpp:1-39`
- `test_rhi_validation.cpp:272-284`
- `frame-and-gpu.md:269-273`

文档：
- MS：设备移除时 fence 被 signal 到 UINT64_MAX；PostThreadMessage 在模态循环中会丢失；QUERYENDSESSION 和 ENDSESSION 的语义。
- Vulkan：
  - 设备丢失时等待在有限时间内返回；
  - VUID-07783；
  - Submit 的错误语义；
  - vkQueuePresentKHR：OOM 时同步对象状态不变，OUT_OF_DATE 等仍算已入队，返回码包括 UNKNOWN；
  - vkQueueWaitIdle 的覆盖范围；
  - present fence 只能通过 VkSwapchainPresentFenceInfoEXT 指定；
  - Vulkan Guide 关于 swapchain semaphore 复用的说明。

B. 推断，或未重新核对
- binary semaphore 的 signal/wait 配对要求：同步章节本轮没有重新查，以你提供的链接为准。
- 同一 fence 允许多个线程同时等待。
- acquire semaphore 不会留下悬挂的 signal。
- DXGI 在 waitable 等待之后调用 Present 的语义。
- D3D12 swapchain 的释放条件，以及 ResizeBuffers 对 back buffer 引用的要求。
- 在 GT 上 Reset allocator 是安全的。
- HRESULT 的映射。
- vkDestroySwapchainKHR 相关的 VUID。
- 销毁已 signal 但没有挂起操作的 binary semaphore 是合法的。
- Submit 返回 UNKNOWN 时提交状态未知。
- ENDSESSION 之后的时序。
- headless 模式下收不到会话消息。
- 事件 FIFO 增长的上界。

C. 助手取舍（可推翻）
- 单个 noexcept 的 OnShutdown；app 的 OnShutdown 只在 OnInit 之后调用；参与者也会收到。
- T0 时正在运行的代码抛出的异常，记为次级失败。
- 终止时 RT 不等 GPU。
- 各请求只能在指定线程调用；RuntimeException 派生自 std::runtime_error。
- RT 在 EndRecord 前检查终止。
- T3b 和 T3c 的非丢失错误归为 L2。
- 屏障用 QueueWaitIdle，T3d 用 DeviceWaitIdle。
- T6 不等待回复；配置无效时返回 Failure(Startup)；由第一个收到 ENDSESSION 的窗口负责处理。
- 帧推进类记录在 stop 时可以从 Ready 转为 Canceled。
- 终止后 Spawn 的 task 直接丢弃。
- 保留 Gpu=None 加 RT 的组合。
- D17 推荐用约束而不是钩子。
- consumer 为 Unknown 时不补空批，交给 T3d。
- InvalidateView 的 abort 保留为 L2 检查。

v5 已完整给出。需要你拍板的是 P4 到 P6。这份稿只在对话里，没有写文件，没有运行测试，也没有保存。