## Application 模块详细需求文档

---

### 1. 概述

#### 1.1 模块职责
Application 是 RadRay 引擎的运行时核心组件，负责：
- 管理 GPU 运行时（GpuRuntime）的创建与销毁
- 管理一个或多个渲染 Surface（Swap Chain）的完整生命周期
- 驱动主循环（逻辑更新 + 渲染调度）
- 提供单线程/多线程渲染两种执行模式
- 通过回调接口将控制权交给上层应用逻辑

#### 1.2 线程模型
- **主线程**：执行主循环、窗口事件处理、逻辑更新、渲染数据提取、状态变更处理
- **渲染线程**（可选）：执行 BeginFrame、OnRender、SubmitFrame/Present

#### 1.3 所有权约定
- Application **借用**外部创建的 `NativeWindow`，不拥有、不管理窗口生命周期
- Application **拥有** GpuRuntime、GpuSurface 及其派生的所有 GPU 对象
- `IAppCallbacks` 指针由外部持有，必须长于 Application 生命周期

---

### 2. 配置

#### 2.1 AppConfig

| 字段 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| Backend | RenderBackend | D3D12 | GPU 后端类型（D3D12 / Vulkan） |
| InitialSurfaces | vector\<AppSurfaceConfig\> | 空 | 启动时创建的 Surface 列表，**至少一个**，**恰好一个 IsPrimary=true** |
| MultiThreadedRender | bool | false | 是否以多线程渲染模式启动 |
| AllowFrameDrop | bool | false | 是否允许帧丢弃（非阻塞 BeginFrame） |

#### 2.2 AppSurfaceConfig

| 字段 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| Window | NativeWindow* | nullptr | 借用的窗口指针，**不可为空**，**必须 IsValid()** |
| SurfaceFormat | TextureFormat | BGRA8_UNORM | Surface 纹理格式 |
| PresentMode | PresentMode | FIFO | 呈现模式（FIFO / Mailbox 等） |
| BackBufferCount | uint32_t | 3 | 后备缓冲区数量，**>0** |
| FlightFrameCount | uint32_t | 2 | 飞行帧数量（in-flight frames），**>0** |
| IsPrimary | bool | false | 是否为主 Surface |

#### 2.3 约束
- `InitialSurfaces` 不可为空
- `InitialSurfaces` 中恰好有一个 `IsPrimary=true`
- `InitialSurfaces` 中不可出现重复的 Window 指针
- 运行时新增 Surface 的 `IsPrimary` 强制设为 false

---

### 3. 生命周期

#### 3.1 构造
```
Application(AppConfig, IAppCallbacks*)
```
- 仅存储配置和回调指针，不做任何初始化

#### 3.2 主循环入口
```
int Run()
```
- 阻塞调用，直到退出
- 返回 0 表示正常退出；返回 1 表示回调异常导致的退出

#### 3.3 启动流程（Run 内部）

| 步骤 | 操作 | 失败处理 |
|---|---|---|
| 1 | BuildInitialSurfaceConfigs：校验 InitialSurfaces 配置合法性 | cleanup → return 1 |
| 2 | CreateRuntime：根据 Backend 创建 GpuRuntime（D3D12 或 Vulkan） | cleanup → return 1 |
| 3 | CreateInitialSurfaces：依次创建所有初始 Surface，初始化 flight slots | cleanup → return 1 |
| 4 | 若 MultiThreadedRender=true，执行 SwitchThreadMode(true) 启动渲染线程 | — |
| 5 | 调用 OnStartup 回调 | 标记 _callbackFailed + _exitRequested |
| 6 | 重置计时器，进入主循环 | — |

#### 3.4 主循环（每帧）

```
while (!_exitRequested) {
    1. DrainRenderCompletions     // 回收渲染线程完成结果
    2. ProcessGpuTasks            // 回收已完成的 GPU 提交
    3. DispatchEvents             // 泵窗口消息（仅一个窗口）
    4. CaptureWindowState         // 捕获所有窗口的关闭/resize 状态
    5. 更新帧信息（DeltaTime, TotalTime, LogicFrameIndex）
    6. OnUpdate 回调
    7. DrainRenderCompletions + ProcessGpuTasks  // 再次回收
    8. HandlePendingChanges       // 执行排队的命令、处理 removal/recreate
    9. DrainRenderCompletions + ProcessGpuTasks  // 再次回收
    10. 检查是否还有活跃 Surface，无则退出
    11. ScheduleSurfaceFrames     // 调度渲染
    12. ProcessGpuTasks
    13. 若无法调度且被 flight slot 阻塞，等待可用 slot
}
```

#### 3.5 关闭流程

| 步骤 | 操作 |
|---|---|
| 1 | WaitRenderIdle：等待渲染线程完成所有排队任务 |
| 2 | DrainRenderCompletions：回收最后的渲染完成结果 |
| 3 | StopRenderThread：发送 StopRenderThreadCommand，join 线程 |
| 4 | GPU Wait：等待 Direct 队列空闲 |
| 5 | ProcessGpuTasks：最终回收 |
| 6 | OnShutdown 回调 |
| 7 | cleanup：释放所有资源、重置所有状态 |

#### 3.6 析构
- `~Application()` 调用 `StopRenderThread()`，确保渲染线程已停止

---

### 4. Surface 管理

#### 4.1 Surface 标识

- `AppSurfaceId`：uint64_t handle，值等于 `_surfaces` 数组下标
- `Invalid()` 使用 `uint64_t::max` 作为哨兵值
- 支持 `<=>` 比较、`std::hash` 特化、`fmt::formatter` 特化

#### 4.2 Surface 状态（SurfaceState）

| 字段 | 说明 |
|---|---|
| Window | 借用的窗口指针 |
| Surface | 拥有的 GpuSurface（swap chain） |
| SurfaceFormat, DesiredPresentMode, BackBufferCount, FlightFrameCount | 配置参数 |
| ActualFlightFrameCount | GpuSurface 实际返回的 flight frame 数量 |
| NextFlightSlotIndex | 下一个待分配的 flight slot 下标（round-robin） |
| LatchedWidth, LatchedHeight | 最近一次捕获的待 resize 尺寸 |
| RenderFrameIndex | 该 Surface 的渲染帧计数器 |
| DroppedFrameCount | 该 Surface 的丢帧计数器 |
| FlightSlots | flight frame slot 数组 |
| Active | 是否活跃 |
| IsPrimary | 是否为主 Surface |
| PendingResize | 是否需要 resize |
| PendingRecreate | 是否需要重建（PresentMode 变更或 SwapChain 状态要求） |
| PendingRemoval | 是否标记待移除 |
| Closing | 窗口正在关闭 |
| OutstandingFrameCount | 非 Free 状态的 flight slot 数量（缓存统计） |

#### 4.3 Surface 创建

**初始创建**（`CreateInitialSurfaces`）：
- 按 `InitialSurfaces` 顺序，SurfaceId = 数组下标
- 不触发 `OnSurfaceAdded` 回调

**运行时添加**（`RequestAddSurface`）：
- 校验：Window 非空且有效、BackBufferCount > 0、FlightFrameCount > 0
- 校验：Window 未被已有 Surface 或排队中的 Add 命令占用
- 分配 SurfaceId（优先复用 inactive slot）
- 投递 `AddSurfaceCommand` 到 `_mainThreadCommands`
- 立即返回 SurfaceId（尚未真正创建）
- 实际创建在 `HandlePendingChanges` 时执行
- 创建成功后触发 `OnSurfaceAdded` 回调

**SurfaceId 分配策略**（`AllocateSurfaceId`）：
1. 优先复用 `Active=false` 的 slot
2. 其次复用 `PendingRemoval=true` 且非 Primary 的 slot（或已有 RemoveCommand 的 slot）
3. 跳过已被排队 Add 命令占用的 slot
4. 若无可复用 slot，分配 `_surfaces.size()` 对应的新 slot

#### 4.4 Surface 移除

**请求移除**（`RequestRemoveSurface`）：
- 若目标存在于排队 Add 命令中，设置 `Cancelled = true`
- 否则投递 `RemoveSurfaceCommand`

**执行移除**（`ExecuteMainThreadCommand(RemoveSurfaceCommand)`）：
- 若目标为 Primary Surface，设置 `_exitRequested = true`（整个 app 退出）
- 否则设置 `PendingRemoval = true`

**最终移除**（`HandlePendingSurfaceRemovals` → `FinalizeSurfaceRemoval`）：
- 条件：`PendingRemoval = true` 且 `OutstandingFrameCount = 0`
- 等待 GPU 队列空闲（`Wait` + `ProcessTasks`）
- 重置 SurfaceState 为默认值
- 触发 `OnSurfaceRemoved` 回调
- 执行 `TrimInactiveSurfaceTail`：移除 `_surfaces` 尾部所有非 Active 的 slot

#### 4.5 Surface 重建

**触发条件**：
- 窗口 resize（`PendingResize = true`）
- PresentMode 变更（`PendingRecreate = true`）
- SwapChain 返回 `RequireRecreate` 状态

**执行条件**：
- Surface Active 且非 PendingRemoval
- `OutstandingFrameCount = 0`（所有 flight frame 已归还）
- Window 有效

**重建流程**（`RecreateSurface`）：
1. 等待 GPU 队列空闲（`Wait` on Direct queue）
2. `ProcessTasks` 回收 pending
3. 销毁旧 Surface
4. 用新尺寸/PresentMode 创建新 Surface
5. 重新初始化 flight slots
6. 触发 `OnSurfaceRecreated` 回调

---

### 5. Flight Frame 系统

#### 5.1 目的
控制 CPU 提前于 GPU 的帧数，防止 CPU 无限超前导致内存压力，同时允许足够的并行度。

#### 5.2 状态机

```
Free ──ReserveFlightSlot──▶ Reserved ──SubmitFlightSlot──▶ Submitted
  ▲                            │                              │
  │                    ReleaseFlightSlot              GpuTask::IsCompleted()
  │                        (异常/丢帧)               RecycleCompletedFlightSlots
  └────────────────────────────┘◀─────────────────────────────┘
```

| 状态 | 含义 |
|---|---|
| Free | 可分配给新帧 |
| Reserved | 已被主线程预订，等待渲染提交 |
| Submitted | 已提交 GPU，绑定了 GpuTask 等待完成 |

#### 5.3 操作

| 操作 | 调用方 | 说明 |
|---|---|---|
| ReserveFlightSlot | 主线程（ScheduleSurfaceFrames） | 从 Free slot 中 round-robin 分配一个 |
| SubmitFlightSlot | 主线程（ProcessRenderCompletion） | 将 GpuTask 绑定到 Reserved slot |
| ReleaseFlightSlot | 主线程 | 将 Reserved/Submitted slot 直接归还为 Free（异常/丢帧时） |
| RecycleCompletedFlightSlots | 主线程（ProcessGpuTasks 后） | 遍历所有 Submitted slot，回收 IsCompleted() 的 |

#### 5.4 等待策略

当所有 flight slot 被占满时：
- **多线程模式**：先等待渲染线程完成至少一个任务（`WaitForRenderProgress`），然后 `DrainRenderCompletions` + `ProcessGpuTasks`
- **所有模式**：遍历所有 Submitted slot，对第一个找到的调用 `GpuTask::Wait()`（CPU 阻塞等待 GPU）

---

### 6. 渲染调度

#### 6.1 调度算法（ScheduleSurfaceFrames）

采用 **round-robin** 从 `_scheduleCursor` 开始扫描所有 Surface：

对每个 Surface，按顺序检查：
1. ❌ 跳过条件：非 Active、PendingRemoval、PendingResize、PendingRecreate、Closing、窗口无效、窗口最小化、Surface 无效
2. ❌ 无可用 flight slot → 设置 `BlockedByFlightSlots = true`，跳过
3. ✅ 预订 flight slot

预订成功后，根据线程模式分支：

**多线程模式**：
1. 主线程调用 `OnExtractRenderData` 提取 `FramePacket`
2. 构造 `RenderSurfaceFrameCommand`（**不含 GpuFrameContext**，由渲染线程自行 BeginFrame）
3. `KickRenderThread` 投递命令

**单线程模式**：
1. `BeginFrame`（或 `TryBeginFrame` if AllowFrameDrop）获取 `GpuFrameContext`
2. 根据 BeginFrame 结果处理：
   - Success：调用 `OnExtractRenderData` → 构造 `RenderSurfaceFrameCommand`（含 GpuFrameContext） → 直接执行 `ExecuteRenderThreadCommand` → `ProcessRenderCompletion`
   - RetryLater：释放 flight slot，`DroppedFrameCount++`
   - RequireRecreate：释放 flight slot，设置 `PendingRecreate`
   - Error：释放 flight slot，`_exitRequested = true`

#### 6.2 帧丢弃（AllowFrameDrop）

- `AllowFrameDrop` 使用 `std::atomic<bool>`，可随时修改
- 开启时使用 `TryBeginFrame`（非阻塞），获取不到 swapchain image 则丢帧
- 关闭时使用 `BeginFrame`（阻塞），flight slot 满时等待

#### 6.3 调度后处理

- 若本帧未调度任何 Surface（`!ScheduledAny`）：
  - 被 flight slot 阻塞 + 不允许丢帧 → 调用 `WaitForAvailableFlightSlot`
  - 多线程模式 + 存在 outstanding jobs → 调用 `WaitForRenderProgress`（避免忙等）

---

### 7. 多线程渲染

#### 7.1 渲染线程 Worker（RenderThreadMain）

```
while (true) {
    1. 在 _renderMutex 下等待 _renderKickCV（条件：_renderQueue 非空）
    2. 弹出一个 RenderThreadCommand
    3. 设置 _renderWorkerBusy = true
    4. 执行命令
    5. 在 _renderMutex 下：推入 RenderCompletion，_renderWorkerBusy = false
    6. notify _renderDoneCV
    7. 若命令结果为 ShouldStop，退出循环
}
```

#### 7.2 渲染命令执行（ExecuteRenderThreadCommand(RenderSurfaceFrameCommand)）

| 步骤 | 操作 |
|---|---|
| 1 | 若 Ctx 为空（多线程模式），在 `_gpuMutex` 下执行 BeginFrame / TryBeginFrame |
| 2 | 调用 `OnRender` 回调（渲染线程），传入 GpuFrameContext 和 FramePacket |
| 3 | 在 `_gpuMutex` 下执行 SubmitFrame（非空 frame）或 AbandonFrame（空 frame / 回调异常） |
| 4 | 构造 RenderCompletion 返回 |

#### 7.3 同步原语

| 原语 | 保护对象 | 说明 |
|---|---|---|
| `_renderMutex` | `_renderQueue`, `_renderCompletions`, `_renderWorkerBusy` | 渲染线程命令队列与完成队列 |
| `_gpuMutex` | `_gpu`（GpuRuntime 的所有操作） | 主线程与渲染线程共享 GPU 操作 |
| `_renderKickCV` | — | 通知渲染线程有新命令 |
| `_renderDoneCV` | — | 通知主线程渲染完成或队列空闲 |

#### 7.4 线程模式切换

- 通过 `SetMultiThreadedRender(bool)` 排队 `SwitchThreadModeCommand`
- 执行时先 `WaitRenderIdle` + `DrainRenderCompletions`，再启动/停止渲染线程
- 可合并：若已有排队的切换命令，直接更新其 Enable 字段

---

### 8. 命令系统

#### 8.1 主线程命令

| 命令 | 来源 | 效果 |
|---|---|---|
| AddSurfaceCommand | `RequestAddSurface` | 创建新 Surface。若目标 slot 正在 PendingRemoval 且有 outstanding frames，defer（返回 false 放回队头） |
| RemoveSurfaceCommand | `RequestRemoveSurface` | Primary → 设置 _exitRequested；Non-primary → 设置 PendingRemoval |
| ChangePresentModeCommand | `RequestPresentModeChange` | 更新 DesiredPresentMode + 设置 PendingRecreate |
| SwitchThreadModeCommand | `SetMultiThreadedRender` | 切换单线程/多线程模式 |
| RequestExitCommand | `RequestExit` | 设置 `_exitRequested = true` |

#### 8.2 命令处理时机
- 在 `HandlePendingChanges` 中集中处理
- `DrainMainThreadCommands`：FIFO 逐个执行，若某个命令返回 false（defer），放回队头并停止处理后续命令
- 两轮执行 `DrainMainThreadCommands` + `HandlePendingSurfaceRemovals` 以处理级联效果

#### 8.3 重复/合并策略
- `SetMultiThreadedRender`：合并到已有的 `SwitchThreadModeCommand`
- `RequestAddSurface`：检查 Window 不重复
- `RequestRemoveSurface`：若目标在排队 Add 中，设置 `Cancelled`
- `RequestPresentModeChange`：若目标在排队 Add 中，直接修改 config；若与当前值和目标值均相同，丢弃

---

### 9. 窗口状态捕获

#### 9.1 CaptureWindowState

每帧对所有 Active Surface 执行：

| 条件 | 处理 |
|---|---|
| Window 为空 / 无效 / ShouldClose | 设置 Closing=true。Primary → _exitRequested=true；Non-primary → PendingRemoval=true |
| 窗口尺寸 ≤ 0 | 跳过（最小化处理） |
| 尺寸变化 | 设置 PendingResize=true，锁存 LatchedWidth/LatchedHeight |

#### 9.2 事件分发
- 仅通过一个窗口调用 `DispatchEvents()`
- 优先使用 Primary Surface 的窗口
- Primary 不可用时，使用第一个有效的 Active Surface 窗口

---

### 10. 回调接口（IAppCallbacks）

| 回调 | 线程 | 调用时机 | 备注 |
|---|---|---|---|
| OnStartup | 主线程 | Runtime 和所有初始 Surface 创建后 | 一次 |
| OnShutdown | 主线程 | 退出主循环后、cleanup 前 | 一次。Runtime 仍存活 |
| OnUpdate | 主线程 | 每帧固定调用 | 传入 delta time |
| OnExtractRenderData | 主线程 | 某 Surface 本帧将进入渲染时 | 返回 FramePacket。多线程+AllowFrameDrop 下 packet 可能被丢弃 |
| OnRender | 单线程=主线程；多线程=渲染线程 | OnExtractRenderData 之后 | 接收 GpuFrameContext + FramePacket |
| OnSurfaceAdded | 主线程 | 运行时新增 Surface 成功后 | 初始 Surface 不触发 |
| OnSurfaceRemoved | 主线程 | Surface 从 Application 注销后 | Window 仍由外部拥有 |
| OnSurfaceRecreated | 主线程 | Surface 重建完成后 | resize / PresentMode 变更 |

#### 10.1 异常处理
- 所有回调均有 try/catch（std::exception + ...）
- 异常后设置 `_callbackFailed = true`、`_exitRequested = true`
- OnExtractRenderData 异常时，已获取的 GpuFrameContext 通过 AbandonFrame 正确回收
- 最终 `Run()` 返回 1

---

### 11. 帧信息

#### 11.1 AppFrameInfo（全局）

| 字段 | 说明 |
|---|---|
| LogicFrameIndex | 逻辑帧计数器，每帧 +1 |
| DeltaTime | 上一帧到本帧的时间间隔（秒） |
| TotalTime | 从启动到当前的累计时间（秒） |

#### 11.2 AppSurfaceFrameInfo（每 Surface 每次渲染）

| 字段 | 说明 |
|---|---|
| RenderFrameIndex | 该 Surface 的渲染帧计数器 |
| DroppedFrameCount | 该 Surface 的累计丢帧数 |
| FlightFrameIndex | 本次使用的 flight slot 下标 |
| FlightFrameCount | 实际的 flight frame 总数 |

---

### 12. 运行时查询 API

| 方法 | 返回 | 说明 |
|---|---|---|
| GetGpuRuntime() | GpuRuntime* | 当前 GPU 运行时 |
| GetWindow() | NativeWindow* | Primary Surface 的窗口 |
| GetWindow(surfaceId) | NativeWindow* | 指定 Surface 的窗口 |
| GetSurface() | GpuSurface* | Primary Surface |
| GetSurface(surfaceId) | GpuSurface* | 指定 Surface |
| GetPrimarySurfaceId() | AppSurfaceId | Primary Surface 的 ID |
| GetSurfaceIds() | vector\<AppSurfaceId\> | 所有 Active Surface 的 ID 列表 |
| HasSurface(surfaceId) | bool | 指定 Surface 是否存在且 Active |
| GetFrameInfo() | const AppFrameInfo& | 当前帧信息 |
| IsMultiThreadedRender() | bool | 当前是否多线程模式 |
| IsFrameDropEnabled() | bool | 当前是否允许丢帧 |
| GetCurrentPresentMode() | PresentMode | Primary Surface 的当前 PresentMode |

---

### 13. 运行时控制 API

所有操作均为主线程调用，**不立即生效**，在下一帧的 `HandlePendingChanges` 中执行。

| 方法 | 说明 |
|---|---|
| SetMultiThreadedRender(bool) | 切换线程模式。合并重复请求 |
| SetAllowFrameDrop(bool) | 切换丢帧模式。立即原子写入，无需排队 |
| RequestAddSurface(config) | 请求添加 Surface。返回预分配的 SurfaceId |
| RequestRemoveSurface(surfaceId) | 请求移除 Surface |
| RequestPresentModeChange(mode) | 变更 Primary Surface 的 PresentMode |
| RequestPresentModeChange(surfaceId, mode) | 变更指定 Surface 的 PresentMode |
| RequestExit() | 请求退出 |

---

### 14. GPU 后端

#### 14.1 D3D12
- 条件编译：`RADRAY_ENABLE_D3D12 && _WIN32`
- Debug 模式（`RADRAY_IS_DEBUG`）启用 Debug Layer 和 GPU-based validation
- 适配器选择：默认自动选择（`AdapterIndex = std::nullopt`）

#### 14.2 Vulkan
- 条件编译：`RADRAY_ENABLE_VULKAN`
- Debug 模式启用 Validation Layer
- 队列配置：一个 Direct queue，count=1
- 物理设备选择：默认自动选择

---

### 15. 退出条件

以下任一条件满足即退出主循环：

| 条件 | 触发方式 |
|---|---|
| RequestExit 调用 | 用户主动请求 |
| Primary Surface 窗口关闭/无效 | CaptureWindowState 中检测 |
| 移除 Primary Surface | ExecuteMainThreadCommand(RemoveSurfaceCommand) |
| 所有 Surface 不再活跃 | 主循环中 HasActiveSurfaces() 返回 false |
| 回调抛出异常 | 任何 IAppCallbacks 方法异常 |
| BeginFrame 返回 Error | ScheduleSurfaceFrames 中 |
| Present 返回 Error | HandlePresentResult 中 |
| Surface 重建失败 | RecreateSurface 中 |
| 渲染线程 BeginFrame 致命错误 | ExecuteRenderThreadCommand 中 `fatalError = true` |