## Application 模块详细设计文档

---

### 1. 架构总览

#### 1.1 分层结构

```
┌─────────────────────────────────────────────────┐
│                 用户应用层                        │
│          (IAppCallbacks 实现)                     │
├─────────────────────────────────────────────────┤
│              Application 运行时                   │
│  ┌──────────┐ ┌──────────┐ ┌──────────────────┐ │
│  │ 主循环    │ │命令系统   │ │ 渲染调度 & 线程  │ │
│  │ 驱动器    │ │(Command  │ │ 管理             │ │
│  │          │ │ Queue)   │ │                  │ │
│  └──────────┘ └──────────┘ └──────────────────┘ │
│  ┌──────────┐ ┌──────────┐ ┌──────────────────┐ │
│  │ Surface  │ │ Flight   │ │ 窗口状态         │ │
│  │ 生命周期  │ │ Frame    │ │ 捕获             │ │
│  │ 管理器    │ │ 管理器   │ │                  │ │
│  └──────────┘ └──────────┘ └──────────────────┘ │
├─────────────────────────────────────────────────┤
│              GpuRuntime (gpu_system.h)           │
│       BeginFrame / SubmitFrame / ProcessTasks    │
├─────────────────────────────────────────────────┤
│          RHI 后端 (D3D12 / Vulkan)               │
└─────────────────────────────────────────────────┘
```

#### 1.2 核心设计原则

| 原则 | 体现 |
|---|---|
| **单一所有权** | Application 拥有 GpuRuntime 和 GpuSurface；借用 NativeWindow 和 IAppCallbacks |
| **延迟执行** | 公共 API 只投递命令到队列，集中在安全时间点执行 |
| **状态机驱动** | Surface 和 flight slot 均用显式状态管理生命周期 |
| **异常隔离** | 回调异常不传播，转化为退出信号 |
| **可选并行** | 渲染线程是可选的，单/多线程共用同一套渲染命令执行路径 |

---

### 2. 数据结构设计

#### 2.1 Surface 存储——槽位数组

```
_surfaces: vector<SurfaceState>

┌───────┬───────┬───────┬───────┬───────┐
│ Slot0 │ Slot1 │ Slot2 │ Slot3 │ Slot4 │
│Active │Active │ (空)  │Active │ (空)  │
│Primary│       │       │       │       │
└───────┴───────┴───────┴───────┴───────┘
   ↑                       ↑
SurfaceId=0            SurfaceId=3
```

**设计选择**：用 vector 下标作为 SurfaceId，而非 map。

**理由**：
- Surface 数量极少（通常 1~4 个），O(n) 遍历成本可忽略
- 下标即 ID，无需额外映射表
- 连续内存布局，缓存友好
- `Active` 标志配合 `TrimInactiveSurfaceTail` 控制 vector 大小

**复用策略**：`AllocateSurfaceId` 优先复用 inactive/pending-removal slot，避免 vector 单调增长。具体优先级：

```
1. !Active 的 slot（已完全释放）
2. PendingRemoval 且非 Primary 的 slot（即将释放，Add 命令会 defer 等待）
3. 队列中有 RemoveCommand 的 slot（更远期的释放）
4. 超出当前 vector 大小的新 slot
```

每一步都跳过已被排队中 `AddSurfaceCommand` 占用的 slot ID。

#### 2.2 Flight Slot 数组

每个 Surface 持有独立的 flight slot 数组：

```
FlightSlots: vector<FlightSlotState>  (大小 = ActualFlightFrameCount)

例：FlightFrameCount=2

┌─────────────┬─────────────┐
│   Slot 0    │   Slot 1    │
│ Status:Free │Status:Submit│
│ Task: null  │ Task:{...}  │
└─────────────┴─────────────┘
     ↑
NextFlightSlotIndex = 0
```

**设计选择**：固定大小数组 + round-robin 分配。

**理由**：
- flight frame 数量在 Surface 创建时确定，运行时不变
- round-robin 确保均匀使用每个 slot，避免某个 slot 反复使用导致 GPU 跨帧依赖
- `NextFlightSlotIndex` 记住上次分配位置，避免每次从 0 开始扫描

#### 2.3 命令队列——双端队列 deque

```
_mainThreadCommands: deque<MainThreadCommand>
_renderQueue:        deque<RenderThreadCommand>   (受 _renderMutex 保护)
_renderCompletions:  deque<RenderCompletion>      (受 _renderMutex 保护)
```

**设计选择**：deque 而非 vector/ring buffer。

**理由**：
- 需要 front 弹出 + back 压入（FIFO）
- `DrainMainThreadCommands` 中命令可能 defer（放回队头），需要 `emplace_front`
- 命令数量极少（每帧 0~数个），容器性能无影响

#### 2.4 命令类型——std::variant

```cpp
using MainThreadCommand = std::variant<
    AddSurfaceCommand,
    RemoveSurfaceCommand,
    ChangePresentModeCommand,
    SwitchThreadModeCommand,
    RequestExitCommand>;

using RenderThreadCommand = std::variant<
    RenderSurfaceFrameCommand,
    StopRenderThreadCommand>;
```

**设计选择**：variant 而非虚函数/继承。

**理由**：
- 命令种类固定、数量少，variant 的类型安全枚举特性更适合
- 无需堆分配，全值语义
- `std::visit` 配合重载集实现分派，编译期保证所有分支都处理

---

### 3. 主循环设计

#### 3.1 帧流水线

```
一帧主循环的完整流水线：

Phase 1: 回收
├─ DrainRenderCompletions     回收渲染线程完成结果 → ProcessRenderCompletion
└─ ProcessGpuTasks            GPU fence 轮询 → RecycleCompletedFlightSlots

Phase 2: 输入与逻辑
├─ DispatchEvents             泵窗口消息
├─ CaptureWindowState         捕获窗口关闭/resize
├─ 更新 FrameInfo             DeltaTime, TotalTime, LogicFrameIndex
└─ OnUpdate 回调

Phase 3: 回收（再次）
├─ DrainRenderCompletions
└─ ProcessGpuTasks

Phase 4: 状态变更
└─ HandlePendingChanges
   ├─ DrainMainThreadCommands（两轮）
   ├─ HandlePendingSurfaceRemovals（两轮）
   └─ HandlePendingSurfaceRecreates

Phase 5: 回收（再次）
├─ DrainRenderCompletions
└─ ProcessGpuTasks

Phase 6: 渲染调度
├─ ScheduleSurfaceFrames      为每个就绪 Surface 调度渲染
├─ ProcessGpuTasks
└─ 等待逻辑（见 3.2）
```

**多次回收的设计理由**：

回收操作（`DrainRenderCompletions` + `ProcessGpuTasks`）在一帧内被调用多次（Phase 1/3/5 + Phase 6 后），目的是：

1. **Phase 1**：在帧开始时回收上一帧的渲染结果，释放 flight slot 供本帧使用
2. **Phase 3**：OnUpdate 期间渲染线程可能完成了新任务，回收后才能正确判断 Surface 的 OutstandingFrameCount
3. **Phase 5**：HandlePendingChanges 可能需要等待 OutstandingFrameCount 归零（Surface recreate/remove），此时再回收一次确保状态最新
4. **Phase 6 后**：调度后立即尝试回收，尤其对单线程模式（调度阶段已同步执行渲染）

#### 3.2 帧尾等待策略

```
ScheduleSurfaceFrames 返回后：

if (!ScheduledAny && !AllowFrameDrop && BlockedByFlightSlots)
    → WaitForAvailableFlightSlot()
      ├─ 多线程：WaitForRenderProgress → DrainCompletions → ProcessGpuTasks
      └─ 通用：遍历 Submitted slot → GpuTask::Wait() → ProcessGpuTasks

else if (多线程 && !ScheduledAny && HasOutstandingSurfaceJobs)
    → WaitForRenderProgress()
      └─ 等待 _renderDoneCV（有新 completion 或 worker 空闲）
```

**设计理由**：

避免在无法调度渲染时忙等（CPU 自旋）。当所有 flight slot 被占满时，需要等待 GPU 完成至少一帧才能继续。分两种情况：

- **被 flight slot 阻塞**：说明 GPU 正在赶工，waiting 是正确行为
- **有未完成 jobs 但未调度新帧**：说明渲染线程还在工作，等待一下避免主线程空转

---

### 4. 线程模型设计

#### 4.1 单线程模式

```
主线程:
  ┌──OnUpdate──┐──ExtractData──┐──BeginFrame──┐──OnRender──┐──Submit+Present──┐
  └────────────┘───────────────┘──────────────┘────────────┘──────────────────┘

所有操作在同一线程按顺序执行。
RenderSurfaceFrameCommand 直接调用 ExecuteRenderThreadCommand，不经过队列。
```

#### 4.2 多线程模式

```
主线程:
  ──[OnUpdate]──[Extract N]──[Kick]──────[OnUpdate]──[Extract N+1]──[Kick]──
                                │                                      │
渲染线程:                       ├──[BeginFrame N]──[OnRender N]──[Submit+Present N]──
                                                                       ├──[Begin N+1]...
```

**关键设计：BeginFrame 在渲染线程执行**

多线程模式下，`RenderSurfaceFrameCommand.Ctx` 为 nullptr。渲染线程在 `ExecuteRenderThreadCommand` 中自行调用 `BeginFrame`。

**理由**：
- BeginFrame 涉及 swapchain acquire，可能阻塞（等待 vsync/image 可用）
- 放在渲染线程可以避免主线程被 acquire 阻塞
- 主线程只负责 Extract，完成后立即返回，最大化逻辑/渲染并行

**单线程模式则反过来**：BeginFrame 在主线程的 `ScheduleSurfaceFrames` 中执行，因为无其他线程可委托。

#### 4.3 渲染线程通信

```
                    _renderMutex
主线程              _renderQueue              渲染线程
  │                     │                       │
  ├── KickRenderThread  │                       │
  │   lock(_renderMutex)│                       │
  │   push command ────▶│                       │
  │   notify _renderKickCV ───────────────────▶ │
  │                     │                  wait(_renderKickCV)
  │                     │                  pop command
  │                     │                  execute...
  │                     │                       │
  │                     │   _renderCompletions  │
  │                     │◀──── push completion  │
  │                     │      _renderWorkerBusy = false
  │ ◀───────────────────────── notify _renderDoneCV
  │   DrainRenderCompletions                    │
  │   lock(_renderMutex)│                       │
  │   swap completions ◀│                       │
  │                     │                       │
```

**设计选择**：单 worker + 单 kick CV + 单 done CV。

**理由**：
- 渲染操作有严格顺序（BeginFrame → Render → Present），多 worker 会引入 GPU 跨线程提交的复杂性
- 单 worker 简化了完成状态的推理：`_renderWorkerBusy` 就能判断 worker 是否空闲
- `_renderDoneCV` 既用于"等待全部完成"(`WaitRenderIdle`)，也用于"等待任意进展"(`WaitForRenderProgress`)，通过不同的 predicate 区分

#### 4.4 GPU 互斥锁设计

```
_gpuMutex 保护 GpuRuntime 的所有方法调用：

主线程加锁点:
  ├─ CreateSurface          (初始化/运行时添加)
  ├─ ProcessTasks           (每帧多次)
  ├─ BeginFrame             (单线程模式调度)
  ├─ AbandonFrame           (单线程模式异常)
  ├─ Wait                   (关闭/Surface 重建)
  └─ CreateSurface/Destroy  (Surface 重建)

渲染线程加锁点:
  ├─ BeginFrame / TryBeginFrame  (多线程模式)
  └─ SubmitFrame / AbandonFrame  (多线程模式)
```

**设计选择**：单一粗粒度互斥锁。

**理由**：
- GpuRuntime 内部状态（pending 队列、fence、registry 等）不保证线程安全
- 加锁粒度以单个 GpuRuntime API 调用为单位
- 简单可靠，避免细粒度锁引入死锁风险

**代价**：
- 主线程 `ProcessGpuTasks` 与渲染线程 `SubmitFrame` 互斥，产生短暂争用
- 这是当前设计中已知的并行度瓶颈

---

### 5. Surface 生命周期状态机

```
                    ┌──────────────┐
                    │  不存在       │
                    │ (slot空/inactive) │
                    └──────┬───────┘
                           │ CreateSurfaceFromConfig
                           ▼
                    ┌──────────────┐
             ┌─────│    Active     │◀────────────────────┐
             │     │ 正常运行       │                      │
             │     └──┬───┬───┬───┘                      │
             │        │   │   │                          │
             │  resize │   │   │ PresentMode变更         │
             │        │   │   │ 或SwapChain返回          │
             │        ▼   │   │ RequireRecreate          │
             │  ┌────────┐│   │                          │
             │  │Pending ││   ▼                          │
             │  │Resize  ││ ┌────────────┐               │
             │  └──┬─────┘│ │  Pending   │               │
             │     │      │ │  Recreate  │               │
             │     │      │ └──┬─────────┘               │
             │     │      │    │                         │
             │     └──────┼────┘                         │
             │            │                              │
             │            │ OutstandingFrameCount==0     │
             │            ▼                              │
             │     HandlePendingSurfaceRecreates          │
             │     ┌──────────────┐                      │
             │     │ RecreateSurface                     │
             │     │ Wait → Destroy → Create → InitSlots │
             │     └──────────┬───┘                      │
             │                │ 成功                     │
             │                ├──────────────────────────┘
             │                │ 失败 → _exitRequested
             │
             │ RemoveCommand / 窗口关闭 / ShouldClose
             ▼
      ┌──────────────┐
      │  Pending     │
      │  Removal     │
      └──────┬───────┘
             │ OutstandingFrameCount==0
             ▼
      ┌──────────────┐
      │ FinalizeSurface│
      │ Removal       │
      │ Wait → Reset → │
      │ OnSurfaceRemoved│
      └──────┬───────┘
             │
             ▼
      ┌──────────────┐
      │  不存在       │
      │ (Active=false)│
      └──────────────┘
```

**关键安全点**：所有 Surface 状态变更（recreate、removal）都要求 **OutstandingFrameCount == 0**，即所有 flight frames 必须先完成。这保证了：
- 渲染线程不会在 Surface 被销毁后访问已释放对象
- GPU 端已完成对该 Surface backbuffer 的使用

---

### 6. Flight Frame 状态机

```
       ┌──────────┐
       │   Free   │◀──────────────────────────────────────┐
       └────┬─────┘                                       │
            │ ReserveFlightSlot                           │
            │ (主线程, ScheduleSurfaceFrames)              │
            ▼                                             │
       ┌──────────┐                                       │
       │ Reserved │──────────────┐                        │
       └────┬─────┘              │                        │
            │                    │ ReleaseFlightSlot       │
            │ ProcessRender-     │ (异常/丢帧/取消)         │
            │ Completion         │                        │
            │ (Success)          │                        │
            ▼                    ▼                        │
       ┌──────────┐         ┌──────────┐                  │
       │Submitted │         │   Free   │                  │
       │ (GpuTask)│         └──────────┘                  │
       └────┬─────┘                                       │
            │ GpuTask::IsCompleted() == true              │
            │ RecycleCompletedFlightSlots                  │
            └─────────────────────────────────────────────┘
```

**Reserved → Free 的多种路径**：

| 原因 | 发生场景 |
|---|---|
| BeginFrame 返回 RetryLater | 单线程模式调度时 swapchain image 不可用 |
| BeginFrame 返回 RequireRecreate | Swapchain 需要重建 |
| BeginFrame 返回 Error | 致命错误 |
| OnExtractRenderData 异常 | 回调抛出异常（此时已有 GpuFrameContext 需要 AbandonFrame） |
| 渲染线程 BeginFrame 失败 | 多线程模式下渲染线程获取不到 frame |

**设计理由**：Reserved 作为中间状态的存在，使得 slot 在 BeginFrame 前就被"锁定"。这允许主线程在调度阶段就知道还剩多少可用 slot，而不必等到渲染线程真正完成 BeginFrame。

---

### 7. 命令系统设计

#### 7.1 两层架构

```
┌─────────────────────────────────────────────────────────┐
│ 第一层：主线程命令 (MainThreadCommand)                     │
│                                                         │
│ 生产者: 用户通过公共 API 投递                               │
│ 消费者: HandlePendingChanges → DrainMainThreadCommands   │
│ 时机: 每帧 Phase 4                                       │
│ 特性: 支持合并、取消、defer                                │
│                                                         │
│   ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐ ┌──────┐ │
│   │  Add   │ │ Remove │ │ Change │ │ Switch │ │ Exit │ │
│   │Surface │ │Surface │ │Present │ │Thread  │ │      │ │
│   └────────┘ └────────┘ └────────┘ └────────┘ └──────┘ │
└─────────────────────────────────────────────────────────┘
                          │
                    执行后可能产生
                          │
                          ▼
┌─────────────────────────────────────────────────────────┐
│ 第二层：渲染线程命令 (RenderThreadCommand)                  │
│                                                         │
│ 生产者: ScheduleSurfaceFrames                            │
│ 消费者: 单线程 → 直接执行; 多线程 → RenderThreadMain       │
│ 时机: 每帧 Phase 6                                       │
│ 特性: 无合并/取消，产生 RenderCompletion 反馈               │
│                                                         │
│   ┌────────────────┐  ┌──────────────────┐              │
│   │ RenderSurface  │  │ StopRenderThread │              │
│   │ FrameCommand   │  │ Command          │              │
│   └────────────────┘  └──────────────────┘              │
└─────────────────────────────────────────────────────────┘
```

#### 7.2 命令 Defer 机制

`DrainMainThreadCommands` 的 FIFO 处理中，命令的 `ExecuteMainThreadCommand` 返回值：
- `true`：命令已完成，弹出并继续下一个
- `false`：命令当前不能执行（例如要添加的 slot 还有 outstanding frames），放回队头并**停止处理后续命令**

**停止后续命令的理由**：命令之间可能有隐含顺序依赖。例如先 Remove A 再 Add B（复用 A 的 slot），如果跳过 Remove 直接执行 Add 会导致状态错误。

#### 7.3 命令合并与去重

| 命令 | 合并策略 |
|---|---|
| SwitchThreadModeCommand | 若已存在排队命令，直接修改其 `Enable` 字段 |
| ChangePresentModeCommand | 当前不合并（允许多个排队），但若新模式等于当前和目标都相同则丢弃 |
| AddSurfaceCommand | 不合并，但检查 Window 不重复（与活跃 Surface 和排队中的 Add 都比较） |
| RemoveSurfaceCommand | 若目标有排队中的 Add，设置 `Cancelled = true` 而非入队 Remove |

---

### 8. 渲染调度设计

#### 8.1 Round-Robin 调度

```
Surface 数组:
  [S0: Active] [S1: Active] [S2: PendingRemoval] [S3: Active]

_scheduleCursor = 1

本帧扫描顺序: S1 → S2(跳过) → S3 → S0

下帧 _scheduleCursor = 2
```

**设计选择**：round-robin 而非固定顺序或优先级。

**理由**：
- 保证多 Surface 公平性，不会出现某个 Surface 永远得不到调度
- `_scheduleCursor` 持久化跨帧，避免"先入队的 Surface 总是先渲染"的偏向

**注意**：当前一帧内所有可调度的 Surface 都会被调度（不限个数）。round-robin 仅影响**遍历起点**，保证在 flight slot 不足时，不总是同一个 Surface 被跳过。

#### 8.2 多线程 vs 单线程渲染路径差异

```
                        ScheduleSurfaceFrames
                               │
                    ┌──────────┴──────────┐
                    │                     │
              _multiThreaded?          !_multiThreaded
                    │                     │
         ┌─────────┴─────────┐      ┌────┴────────┐
         │ 主线程:             │      │ 主线程:       │
         │ 1. ReserveSlot     │      │ 1. ReserveSlot│
         │ 2. ExtractData     │      │ 2. BeginFrame │
         │ 3. KickRenderThread│      │ 3. ExtractData│
         │    (Ctx=nullptr)   │      │ 4. Execute    │
         │                    │      │    直接调用     │
         │ 渲染线程:           │      │ 5. Process    │
         │ 4. BeginFrame      │      │    Completion  │
         │ 5. OnRender        │      └───────────────┘
         │ 6. Submit+Present  │
         │ 7→ Completion      │
         └────────────────────┘
```

**差异关键点**：

| 方面 | 单线程 | 多线程 |
|---|---|---|
| BeginFrame 位置 | 主线程 ScheduleSurfaceFrames 中 | 渲染线程 ExecuteRenderThreadCommand 中 |
| Ctx 传递 | 通过 RenderSurfaceFrameCommand.Ctx（非空） | RenderSurfaceFrameCommand.Ctx = nullptr |
| OnRender 线程 | 主线程 | 渲染线程 |
| Completion 处理 | 直接调用 ProcessRenderCompletion | 推入 _renderCompletions，主线程 DrainRenderCompletions 回收 |
| BeginFrame 失败处理 | ScheduleSurfaceFrames 中直接处理 | ExecuteRenderThreadCommand 中处理，通过 Completion 回传 |

#### 8.3 帧丢弃机制

```
AllowFrameDrop = true 时：

使用 TryBeginFrame (非阻塞)
    │
    ├─ Success     → 正常渲染
    ├─ RetryLater  → DroppedFrameCount++，释放 slot，跳过本 Surface
    ├─ RequireRecreate → PendingRecreate = true
    └─ Error       → _exitRequested = true

AllowFrameDrop = false 时：

使用 BeginFrame (阻塞)
    │
    └─ 等待直到 swapchain image 可用
```

**AllowFrameDrop 使用 atomic<bool>**：允许从任意线程修改（`SetAllowFrameDrop`），渲染线程在 BeginFrame 时读取。语义上是 relaxed ordering，因为丢帧与否不涉及与其他变量的 happens-before 关系。

---

### 9. 窗口状态与事件设计

#### 9.1 事件分发模型

```
当前设计：单窗口消息泵

GetEventDispatchWindow()
    ├─ 优先: Primary Surface 的 Window
    └─ 回退: 第一个 Active Surface 的 Window

DispatchEvents() 调用该窗口的消息泵
```

**设计理由**：
- Win32 的 `PeekMessage`/`DispatchMessage` 是 per-thread（不是 per-window），一次泵即可处理该线程所有窗口的消息
- 只需调用一次即可
- 优先使用 Primary 窗口，因为它最可能存在

#### 9.2 Resize 锁存

```
CaptureWindowState 中：

当前 Surface 尺寸 ←──→ Window 实际尺寸

如果不一致：
    PendingResize = true
    LatchedWidth = 实际宽
    LatchedHeight = 实际高

如果已是 PendingResize 但尺寸又变了：
    更新 LatchedWidth/LatchedHeight（取最新值）
```

**设计理由**：resize 期间用户可能连续拖动窗口边缘，产生多次尺寸变化。通过锁存只记录最新值，避免频繁重建 swap chain。真正的重建延迟到 `OutstandingFrameCount == 0` 时一次性执行。

---

### 10. 错误处理设计

#### 10.1 回调异常处理策略

```
try {
    _callbacks->OnXxx(...);
} catch (const std::exception& ex) {
    _LogCallbackException("OnXxx", ex);
    _callbackFailed = true;
    _exitRequested = true;
} catch (...) {
    _LogUnknownCallbackException("OnXxx");
    _callbackFailed = true;
    _exitRequested = true;
}
```

**统一策略**：所有 7 个回调均有此包裹。异常不会传播到 Application 内部逻辑，而是转化为退出信号。

**特殊处理**：`OnExtractRenderData` 异常时，如果已获取了 `GpuFrameContext`（单线程模式的 BeginFrame 已成功），必须调用 `AbandonFrame` 合法收口 swapchain。渲染线程侧的 `OnRender` 异常也同理，通过 `AbandonFrame` 收口。

#### 10.2 GPU 错误处理

| 错误来源 | 处理方式 |
|---|---|
| BeginFrame 返回 Error | `_exitRequested = true` |
| Present 返回 Error | `_exitRequested = true` |
| RecreateSurface 创建失败 | `_exitRequested = true` |
| 渲染线程 BeginFrame 异常 | `fatalError = true` → Completion 回传 → `_exitRequested = true` |
| GpuRuntime 创建失败 | `Run()` 返回 1 |

**设计理由**：GPU 错误通常不可恢复（设备丢失、硬件故障等），统一走退出路径。可恢复的情况（如 `RequireRecreate`）走 Surface 重建路径。

#### 10.3 退出码

```
Run() 返回值:
  0 = 正常退出
  1 = _callbackFailed 为 true（任一回调抛了异常）
```

---

### 11. 同步与并发控制

#### 11.1 锁的层次

```
锁获取顺序（必须遵守以避免死锁）：

  _renderMutex 先于 _gpuMutex

理由：
  渲染线程在 RenderThreadMain 中先持有 _renderMutex（从队列取命令），
  然后在 ExecuteRenderThreadCommand 中获取 _gpuMutex（执行 GPU 操作）。
  主线程不会在持有 _gpuMutex 时获取 _renderMutex。
```

#### 11.2 condition_variable 使用

| CV | 等待方 | 通知方 | 语义 |
|---|---|---|---|
| `_renderKickCV` | 渲染线程 | 主线程 KickRenderThread | 队列非空 |
| `_renderDoneCV` | 主线程 | 渲染线程 | 有以下任一情况发生变化 |

`_renderDoneCV` 的 predicate 根据使用场景不同：

```cpp
// WaitRenderIdle: 等待所有任务完成
_renderDoneCV.wait(lock, [this] {
    return _renderQueue.empty() && !_renderWorkerBusy;
});

// WaitForRenderProgress: 等待任意进展
_renderDoneCV.wait(lock, [this] {
    return !_renderCompletions.empty() || (_renderQueue.empty() && !_renderWorkerBusy);
});
```

#### 11.3 DrainRenderCompletions 的 swap 技巧

```cpp
void Application::DrainRenderCompletions() {
    deque<RenderCompletion> completions{};
    {
        std::lock_guard lock(_renderMutex);
        completions.swap(_renderCompletions);  // O(1) swap
    }
    // 锁外处理所有 completion
    for (auto& completion : completions) {
        ProcessRenderCompletion(std::move(completion));
    }
}
```

**设计理由**：最小化持锁时间。用 swap 将所有 completion 一次性取出，然后在无锁环境下逐一处理。`ProcessRenderCompletion` 可能调用回调、修改 Surface 状态等耗时操作，不应在持锁时执行。

---

### 12. 数据流图

#### 12.1 多线程模式一帧渲染的完整数据流

```
                     主线程                              渲染线程
                       │                                   │
            ┌──────────┴──────────┐                        │
            │    OnUpdate(dt)     │                        │
            └──────────┬──────────┘                        │
                       │                                   │
            ┌──────────┴──────────┐                        │
            │ ScheduleSurfaceFrames│                        │
            │  ReserveFlightSlot  │                        │
            │  OnExtractRenderData│──→ FramePacket         │
            │                     │                        │
            │  KickRenderThread   │                        │
            │    ┌────────────────┤                        │
            │    │ RenderSurface  │                        │
            │    │ FrameCommand   │                        │
            │    │  .SurfaceId    │                        │
            │    │  .Surface*     │                        │
            │    │  .Ctx=nullptr  │   ──push──▶            │
            │    │  .AppInfo      │             _renderQueue│
            │    │  .SurfaceInfo  │                        │
            │    │  .Packet       │                        │
            │    └────────────────┤         wait on        │
            │                     │       _renderKickCV    │
            └──────────┬──────────┘              │         │
                       │                         ├─────────┤
                       │                         │ pop cmd │
               （主线程继续下一帧               │         │
                或等待 flight slot）            │ lock _gpuMutex
                       │                         │ BeginFrame → GpuFrameContext
                       │                         │ unlock _gpuMutex
                       │                         │         │
                       │                         │ OnRender(ctx, packet)
                       │                         │         │
                       │                         │ lock _gpuMutex
                       │                         │ SubmitFrame → GpuTask + PresentResult
                       │                         │ unlock _gpuMutex
                       │                         │         │
                       │                         │ lock _renderMutex
                       │                         │ push RenderCompletion
                       │                         │   .SurfaceId
                       │                         │   .BeginStatus
                       │                         │   .PresentResult
                       │                         │   .FlightFrameIndex
                       │                         │   .Task
                       │                         │ _renderWorkerBusy = false
                       │                         │ unlock _renderMutex
                       │            ◀────────────│ notify _renderDoneCV
                       │                         │
            ┌──────────┴──────────┐              │
            │DrainRenderCompletions│              │
            │ swap _renderCompletions              │
            │ ProcessRenderCompletion              │
            │  SubmitFlightSlot(task)              │
            │  HandlePresentResult                │
            └─────────────────────┘              │
```

#### 12.2 FramePacket 数据流

```
                  主线程                     渲染线程
                    │                          │
  OnExtractRenderData()                        │
    return unique_ptr<FramePacket>              │
                    │                          │
    move into RenderSurfaceFrameCommand.Packet │
                    │                          │
                 ──push──▶ _renderQueue ──pop──▶
                                               │
                              OnRender(ctx, packet.get())
                                               │
                         Packet 在 command 析构时销毁
                         (渲染线程)
```

**FramePacket 的所有权转移**：
1. `OnExtractRenderData` 在主线程创建并返回 `unique_ptr<FramePacket>`
2. Move 进入 `RenderSurfaceFrameCommand`
3. Command move 进入 `_renderQueue`
4. 渲染线程 pop 后通过 `packet.get()` 传给 `OnRender`
5. Command 在渲染线程被析构时自动销毁 Packet

---

### 13. 关键设计决策汇总

| 决策 | 选项 | 选择 | 理由 |
|---|---|---|---|
| SurfaceId 类型 | 数组下标 vs 生成式 ID | 数组下标 | Surface 数量极少，下标简单高效 |
| 命令系统 | variant vs 虚函数 vs enum+union | variant | 类型安全、值语义、编译器分派 |
| Surface 存储 | vector vs map | vector | Cache 友好、下标即 ID |
| 渲染线程数 | 1 vs N | 1 | GPU 提交有顺序约束，多线程增加复杂度 |
| GPU 锁 | 粗粒度 vs 细粒度 | 粗粒度 | 简单、不需要理解 GpuRuntime 内部结构 |
| BeginFrame 位置（多线程） | 主线程 vs 渲染线程 | 渲染线程 | 避免主线程被 swapchain acquire 阻塞 |
| 帧丢弃控制 | atomic vs 命令队列 | atomic | 语义简单，需要立即生效 |
| 错误传播 | 异常 vs 错误码 | 混合（回调异常捕获→退出信号，GPU 错误码→退出信号） | 隔离用户代码的不可控异常 |
| Resize 策略 | 立即重建 vs 锁存延迟 | 锁存延迟 | 避免拖动窗口时频繁重建 |
| Completion 回收 | 逐个回收 vs 批量 swap | 批量 swap | 最小化持锁时间 |