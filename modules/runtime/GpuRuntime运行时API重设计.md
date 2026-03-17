# GpuRuntime 运行时 API 重设计

## 1. 目的

本文档用于固定 `modules/runtime` 层的新 API 语义。

目标不是立刻给出完整实现，而是先明确以下问题：

- `GpuRuntime` 在整个渲染架构中的定位
- `GpuFrameContext`、`GpuAsyncContext`、`GpuTask` 的固定职责
- 多 swap chain / 多窗口场景下的统一语义
- 帧驱动渲染与脱离帧的异步 GPU 工作如何共存
- 哪些能力属于 runtime，哪些能力应留给 Render Graph 或更高层 renderer

本文档确定后，后续代码实现必须以本文档为准，而不是继续沿用当前 `GpuSubmitContext` 风格的模糊语义。

---

## 2. 设计结论

新的 `GpuRuntime` 固定定位为：

**Render Graph 的执行基础设施层，同时也是渲染系统与其他系统交互的 GPU 边界层。**

它不是：

- 高层 renderer feature API
- scene / material / pass 语义层
- 逻辑渲染编排层
- 所有 GPU 业务都直接堆进去的 God Object

它负责：

- 管理设备、队列、提交、回收、同步
- 定义帧上下文与脱离帧的异步上下文
- 统一 surface / swap chain / present 生命周期
- 统一 GPU 任务的等待、查询和依赖表达
- 为 Render Graph 提供稳定的执行宿主
- 作为 window、imgui、多窗口、资源上传系统等外部系统的 GPU 边界

它不负责：

- 决定有哪些渲染 pass
- 决定资源依赖图
- 决定 renderer 的功能结构
- 让 feature author 直接围绕 fence / submit / swapchain 写业务逻辑

---

## 3. 分层

固定为以下四层：

### 3.1 `render`

底层 RHI / backend 抽象层。

职责：

- `Device`
- `CommandQueue`
- `CommandBuffer`
- `Fence`
- `SwapChain`
- `Buffer` / `Texture`

这一层不承载帧语义，也不承载 graph 语义。

### 3.2 `runtime`

即本文档设计的 `GpuRuntime` 层。

职责：

- 帧执行边界
- 异步计算边界
- surface 生命周期
- GPU 任务与依赖
- 提交与退休回收
- 与其他系统的 GPU 交互边界

### 3.3 `render graph`

逻辑资源与 pass 依赖层。

职责：

- pass 描述
- 资源读写声明
- barrier 推导
- transient 生命周期分析
- 编译成 runtime 可执行计划

### 3.4 `renderer`

真正的渲染功能层。

职责：

- 阴影
- gbuffer
- lighting
- postprocess
- ui composition

---

## 4. 核心原则

### 4.1 帧主导，不等于帧唯一

渲染系统以 `GpuFrameContext` 作为主导执行单位。

但不是所有 GPU 工作都必须属于某一帧。

系统同时支持：

- 帧内可见渲染与 frame-local async compute
- 脱离帧的异步 GPU 工作

### 4.2 `GpuTask` 是完成令牌，不是工作描述

`GpuTask` 固定表示：

- 一次已提交 GPU 工作的完成句柄
- 一个可等待、可查询、可依赖的同步对象
- 一个**统一完成语义**的外部对象

它不是：

- command buffer
- pass
- graph node
- 长期业务任务对象

### 4.3 surface 生命周期与提交完成语义解耦

swap chain 的 acquire / present / resize / lost 属于 surface 语义。

GPU 提交完成属于 submission 语义。

两者不能再通过“某块 surface 的 fence 顺便充当整次提交完成点”的方式耦合。

### 4.4 `GpuTask` 必须支持多队列统一完成点

一个 frame 或 detached async 任务，内部可能需要落到多个队列：

- graphics queue
- compute queue
- copy queue

但对外语义必须仍然只有一个 `GpuTask`。

因此固定要求：

- `GpuTask` 对外表现为一个完成令牌
- `GpuTask` 内部允许由多个精确 completion point 共同组成
- `GpuTask::IsCompleted()` / `Wait()` 的语义是“所有内部 completion point 都完成”

### 4.5 多 surface 是一等场景

一帧可以：

- 不获取任何 surface
- 获取一块 surface
- 获取多块 surface
- 只 present 其中一部分 surface

这对于 ImGui 多 viewport、多窗口编辑器、离屏工具窗口都是必需能力。

### 4.6 runtime 管物理执行，graph 管逻辑依赖

`GpuRuntime` 只负责“怎么执行”。

Render Graph 负责“为什么这样执行”。

两层必须分清。

---

## 5. 核心对象

新 API 固定以下五个主对象：

- `GpuRuntime`
- `GpuPresentSurface`
- `GpuFrameContext`
- `GpuAsyncContext`
- `GpuTask`

同时引入一个新的 frame-local 边界对象：

- `GpuSurfaceLease`

并引入一个 runtime 内部同步抽象：

- `GpuCompletionState`

---

## 6. GpuRuntime

### 6.1 定位

`GpuRuntime` 是设备级运行时对象。

它是：

- Render Graph 的执行宿主
- 所有 GPU 提交的统一入口
- surface 管理与其他系统交互的正式边界

### 6.2 职责

- 创建与销毁 device
- 创建 `GpuPresentSurface`
- 创建 `GpuFrameContext`
- 创建 `GpuAsyncContext`
- 提交 frame/async context
- 管理 in-flight submission 生命周期
- 管理 GPU 退休回收
- 管理跨系统可见的 GPU 任务

### 6.3 不负责的内容

- 不直接暴露 renderer feature API
- 不持有 scene / material / camera 语义
- 不决定 pass 拓扑
- 不充当 graph compiler

### 6.4 建议入口

```cpp
class GpuRuntime {
public:
    static Nullable<unique_ptr<GpuRuntime>> Create(const GpuRuntimeDescriptor& desc) noexcept;

    Nullable<unique_ptr<GpuPresentSurface>> CreatePresentSurface(
        const GpuPresentSurfaceDescriptor& desc) noexcept;

    Nullable<unique_ptr<GpuFrameContext>> BeginFrame() noexcept;
    Nullable<unique_ptr<GpuAsyncContext>> BeginAsync() noexcept;

    GpuTask Submit(unique_ptr<GpuFrameContext> frame) noexcept;
    GpuTask Submit(unique_ptr<GpuAsyncContext> asyncContext) noexcept;

    void ProcessTasks() noexcept;
    void Destroy() noexcept;
};
```

### 6.5 `ProcessTasks()` 的固定语义

`ProcessTasks()` 只做一件事：

- 非阻塞处理已经提交任务的退休与资源回收

它不做：

- graph 编译
- 强制等待
- 自动 acquire / present

---

## 7. GpuPresentSurface

### 7.1 定位

`GpuPresentSurface` 表示一个长期存活的 swap chain / present surface。

它与 frame 解耦，与 device 绑定，与窗口系统或其他 native present target 绑定。

### 7.2 固定职责

- 持有 swap chain
- 持有 surface 自己的尺寸、格式、present mode
- 管理 surface 自己的 acquire / present pacing
- 管理 resize / out-of-date / lost 状态
- 管理每个 flight slot 的退休完成点

### 7.3 固定约束

- `GpuPresentSurface` 本身不是一帧
- `GpuPresentSurface` 本身不是提交对象
- surface 不为整次 GPU 提交提供完成 fence 语义
- 一块 surface 可以被不同帧上下文在不同时间获取

### 7.4 多 surface 语义

系统必须原生支持同时存在多块 `GpuPresentSurface`。

典型场景：

- 主窗口
- ImGui 多 viewport 子窗口
- 编辑器工具窗口
- 额外监视器输出窗口

### 7.5 建议接口

```cpp
class GpuPresentSurface {
public:
    bool IsValid() const noexcept;
    bool Reconfigure(
        uint32_t width,
        uint32_t height,
        std::optional<render::TextureFormat> format = std::nullopt,
        std::optional<render::PresentMode> presentMode = std::nullopt) noexcept;

    uint32_t GetWidth() const noexcept;
    uint32_t GetHeight() const noexcept;
    render::TextureFormat GetFormat() const noexcept;
    render::PresentMode GetPresentMode() const noexcept;

    void Destroy() noexcept;
};
```

---

## 8. GpuSurfaceLease

### 8.1 引入原因

当前 API 用 `BackBuffer Handle + Surface` 组合表达 present 权利，语义不够稳。

新的设计中，surface acquire 的结果必须是一个专门的 frame-local 对象：

`GpuSurfaceLease`

### 8.2 固定语义

`GpuSurfaceLease` 表示：

- 某个 `GpuFrameContext` 在当前帧内对某块 surface 某个 back buffer 的独占使用权
- 该权利只能在当前 frame context 生命周期内有效
- 该对象既不是普通资源句柄，也不是长期资源

### 8.3 职责

- 封装 acquire 结果
- 表达当前 back buffer 的 present 权利
- 作为 render graph 导入 external backbuffer 的入口

### 8.4 固定约束

- `GpuSurfaceLease` 不能跨帧保存
- `GpuSurfaceLease` 不能传给 `GpuAsyncContext`
- `GpuSurfaceLease` 不能脱离所属 `GpuFrameContext` 使用
- `Present()` 只标记该 lease 需要 present，而不是重新靠 surface + handle 配对

### 8.5 所有权模型

`GpuSurfaceLease` 的所有权固定如下：

- lease 由 `GpuFrameContext` 内部持有
- `AcquireSurface()` / `TryAcquireSurface()` 只返回借用指针
- 调用方不能接管 lease 生命周期
- `Present()` 不转移所有权，只在 frame context 内标记该 lease 需要 present

### 8.6 Acquire 状态

引入专用状态枚举：

```cpp
enum class GpuSurfaceAcquireStatus : uint8_t {
    Success,
    Unavailable, // 仅 TryAcquire 使用，表示非阻塞获取失败
    Suboptimal,
    OutOfDate,
    Lost,
};
```

固定语义：

- `Success`：成功获得可绘制 back buffer
- `Unavailable`：当前没有可立即获取的 image，但 surface 仍然有效
- `Suboptimal`：可以继续使用，但应尽快重建
- `OutOfDate`：本次无法继续使用，必须重建
- `Lost`：surface 已失效，必须销毁并重建

### 8.7 建议结果对象

```cpp
struct GpuSurfaceAcquireResult {
    GpuSurfaceAcquireStatus Status{GpuSurfaceAcquireStatus::Lost};
    Nullable<GpuSurfaceLease*> Lease{nullptr};
};
```

---

## 9. GpuFrameContext

### 9.1 定位

`GpuFrameContext` 是一帧 GPU 工作的执行上下文，并且**为 frame / present / surface 域特化**。

它是整个渲染系统的主导上下文。

### 9.2 适用范围

它承载：

- 本帧 graphics work
- 本帧需要的 frame-local async compute
- 0..N 个 surface acquire/present
- 本帧 graph 编译结果的执行宿主

它不承载：

- 脱离帧的长期异步 GPU 工作
- surface 之外的长期后台 GPU 任务

### 9.3 固定职责

- 管理本帧依赖的 `GpuTask`
- 持有并管理一个或多个 `GpuSurfaceLease`
- 导入本帧的 present target
- 接收 Render Graph 编译产物并执行
- 在提交后生成一个 `GpuTask`
- 作为 frame pacing 与多 surface present 的统一语义边界

### 9.4 固定约束

- 一个 `GpuFrameContext` 只允许 `Submit()` 一次
- 一个 `GpuFrameContext` 可以不获取任何 surface
- 一个 `GpuFrameContext` 可以获取多块 surface
- 一个 `GpuFrameContext` 的 transient 资源不能跨帧泄露
- `GpuFrameContext` 围绕 frame/present 域建模，但不等于“必须绑定一个 swap chain”

### 9.5 与 Render Graph 的关系

`GpuFrameContext` 是 Render Graph 的执行宿主，而不是 Graph 本身。

推荐流程：

1. `GpuRuntime::BeginFrame()`
2. `GpuFrameContext::AcquireSurface()` / `TryAcquireSurface()`
3. Render Graph 基于 frame context 构建与编译
4. runtime 执行 frame context
5. `GpuRuntime::Submit(frame) -> GpuTask`

### 9.6 建议接口

```cpp
class GpuFrameContext {
public:
    GpuSurfaceAcquireResult AcquireSurface(GpuPresentSurface& surface) noexcept;
    GpuSurfaceAcquireResult TryAcquireSurface(GpuPresentSurface& surface) noexcept;

    bool Present(GpuSurfaceLease& lease) noexcept;
    bool WaitFor(const GpuTask& task) noexcept;
    bool AddGraphicsCommandBuffer(unique_ptr<render::CommandBuffer> cmd) noexcept;
    bool AddComputeCommandBuffer(unique_ptr<render::CommandBuffer> cmd) noexcept;
    bool AddCopyCommandBuffer(unique_ptr<render::CommandBuffer> cmd) noexcept;
    bool SetSubmissionPlan(vector<GpuQueueSubmitStep> steps) noexcept;
    bool IsEmpty() const noexcept;

    Nullable<unique_ptr<render::CommandBuffer>> CreateGraphicsCommandBuffer() noexcept;
    Nullable<unique_ptr<render::CommandBuffer>> CreateComputeCommandBuffer() noexcept;
    Nullable<unique_ptr<render::CommandBuffer>> CreateCopyCommandBuffer() noexcept;
};
```

### 9.7 `AcquireSurface()` 与 `TryAcquireSurface()` 的固定区别

- `AcquireSurface()`：
  - 阻塞
  - 适合主窗口或必须保证交互可见的 surface

- `TryAcquireSurface()`：
  - 非阻塞
  - 适合 ImGui 多 viewport、工具窗口、辅助输出窗口
  - 返回 `Unavailable` 不表示错误

### 9.8 多 surface 语义

一帧中：

- 可以只渲染主窗口
- 可以同时渲染主窗口和若干 ImGui 子窗口
- 某个 surface acquire 失败，不应自动让整帧失败
- 每块 surface 的 acquire 状态必须独立处理

### 9.9 frame-local async compute

`GpuFrameContext` 允许存在 frame-local async compute。

固定语义：

- 这类 compute 工作从属于当前帧
- 它们的资源生命周期从属于当前帧 / 当前 graph
- 它们的完成性由本帧 `GpuTask` 统一覆盖

### 9.10 帧内跨队列依赖

`GpuFrameContext` 可以同时包含 graphics / compute / copy 工作，但 runtime **不负责猜测**这些工作之间的物理提交顺序。

固定语义：

- Render Graph 或等价的 frame compiler 负责生成显式提交计划
- 该计划必须明确：
  - 每个 submission step 归属哪个队列
  - 每个 step 提交哪些命令
  - 每个 step 需要等待哪些前置 step 完成
- `GpuRuntime::Submit(frame)` 只负责执行这个计划，而不是从“命令属于哪个队列类别”反推物理依赖

也就是说：

- graph 负责逻辑依赖
- compiler 负责把逻辑依赖编译成物理 submission plan
- runtime 负责按物理 submission plan 提交

### 9.11 单队列默认语义

在没有显式 submission plan 之前，`GpuFrameContext` 的保守默认语义应为：

- 只允许单队列执行
- 或者由 runtime 明确退化为“统一落到 graphics queue”

在没有显式提交计划时，runtime 不应假设 graphics / compute / copy 可以任意顺序提交。

这条默认语义的正式落点是：

- 通过 `Add*CommandBuffer()` 进入单队列简化路径
- `Submit()` 在内部自动构造单 step plan

而不是“只要调用过 `Create*CommandBuffer()`，runtime 就自动猜测该怎么提交”。

### 9.12 `Create*CommandBuffer()` 与 `SetSubmissionPlan()` 的固定交互

`GpuFrameContext` 固定提供两条命令提交流程：

#### A. 显式 plan 路径

用于：

- Render Graph compiler
- 多队列提交
- 需要跨队列依赖的 frame

固定流程：

1. `Create*CommandBuffer()`
2. 录制命令
3. 将 command buffer 放入 `GpuQueueSubmitStep`
4. 调用 `SetSubmissionPlan(...)`

#### B. 单队列简化路径

用于：

- 单队列测试
- demo / sample
- 尚未接入 graph compiler 的早期开发

固定流程：

1. `Create*CommandBuffer()`
2. 录制命令
3. 调用对应的 `AddGraphicsCommandBuffer()` / `AddComputeCommandBuffer()` / `AddCopyCommandBuffer()`
4. `Submit()` 内部自动把这些命令收敛成一个单 step submission plan

固定约束：

- 同一个 `GpuFrameContext` 中，`Add*CommandBuffer()` 与 `SetSubmissionPlan()` 互斥
- 如果已经使用了 `Add*CommandBuffer()`，后续 `SetSubmissionPlan()` 必须失败
- 如果已经设置了 `SetSubmissionPlan()`，后续 `Add*CommandBuffer()` 也必须失败
- 单队列简化路径下，只允许实际使用一个队列类型
- 一旦需要多队列或跨队列依赖，必须切换到显式 plan 路径

这意味着 `Create*CommandBuffer()` 的返回值必须最终被以下两种方式之一接管：

- 进入 `GpuQueueSubmitStep`，然后交给 `SetSubmissionPlan()`
- 进入对应的 `Add*CommandBuffer()`

不允许存在“创建了命令，但没有被任何提交路径接管”的模糊状态。

---

## 10. GpuAsyncContext

### 10.1 定位

`GpuAsyncContext` 表示脱离帧的异步 GPU 上下文。

它用于：

- 后台预计算
- 与可见帧无直接绑定的离屏 graphics / compute / copy 任务
- 未来可能的长时异步 GPU 数据生成

### 10.2 固定语义

`GpuAsyncContext` 不属于任何可见 frame。

它的生命周期与 swap chain、present、back buffer 无关。

### 10.3 固定职责

- 构建 detached async work
- 声明对其他 `GpuTask` 的依赖
- 提交后返回 `GpuTask`

### 10.4 固定约束

- `GpuAsyncContext` 不允许 acquire surface
- `GpuAsyncContext` 不允许 present
- `GpuAsyncContext` 不允许使用 frame-transient 资源
- `GpuAsyncContext` 的资源必须是 runtime 持久资源或显式导入资源

### 10.5 与 frame 的关系

`GpuAsyncContext` 生成的 `GpuTask` 可以在未来某一帧中被 `GpuFrameContext::WaitFor()` 消费。

这就是“帧主导，不等于帧唯一”的固定落点。

### 10.6 建议接口

```cpp
class GpuAsyncContext {
public:
    bool WaitFor(const GpuTask& task) noexcept;
    bool AddGraphicsCommandBuffer(unique_ptr<render::CommandBuffer> cmd) noexcept;
    bool AddComputeCommandBuffer(unique_ptr<render::CommandBuffer> cmd) noexcept;
    bool AddCopyCommandBuffer(unique_ptr<render::CommandBuffer> cmd) noexcept;
    bool SetSubmissionPlan(vector<GpuQueueSubmitStep> steps) noexcept;
    bool IsEmpty() const noexcept;
    Nullable<unique_ptr<render::CommandBuffer>> CreateGraphicsCommandBuffer() noexcept;
    Nullable<unique_ptr<render::CommandBuffer>> CreateComputeCommandBuffer() noexcept;
    Nullable<unique_ptr<render::CommandBuffer>> CreateCopyCommandBuffer() noexcept;
};
```

### 10.7 为什么改成 `GpuAsyncContext`

既然脱离帧工作已经明确需要支持 graphics，继续使用 `GpuComputeContext` 这个名字会让命名和真实能力长期冲突。

因此固定改为：

- `GpuAsyncContext` 表示脱离帧的异步 GPU 工作
- 它允许 detached graphics / compute / copy
- 它依然不允许 acquire surface、present 和 frame-transient 资源

理由是：

- 真正的分层边界是“是否属于 frame/present 域”，而不是“是否恰好只跑 compute”
- 后台离屏 raster、thumbnail、工具预渲染、blit、mipmap 生成都可能是合法的 detached work
- 只要继续禁止 surface/present/frame-transient，`GpuAsyncContext` 仍然不会退化回旧式无边界的 `GpuSubmitContext`

### 10.8 命令提交流程

`GpuAsyncContext` 与 `GpuFrameContext` 在命令提交流程上保持一致：

- 简单单队列场景可走 `Add*CommandBuffer()` 简化路径
- 多队列或显式依赖场景必须走 `SetSubmissionPlan()` 路径
- 两条路径在同一个 context 内互斥

---

## 11. GpuTask

### 11.1 保留命名

`Submit()` 的返回结果仍固定命名为 `GpuTask`。

### 11.2 固定语义

`GpuTask` 表示一次已提交 GPU 工作的完成令牌。

固定约束：

- 一个 `GpuTask` 对应一次 `GpuRuntime::Submit(...)`
- 它不是 command buffer
- 它不是 pass
- 它不是 graph node
- 它不是 surface present 完成语义
- 它内部可以由一个或多个精确 completion point 组成

### 11.3 来源

`GpuTask` 可以来自：

- `GpuRuntime::Submit(unique_ptr<GpuFrameContext>)`
- `GpuRuntime::Submit(unique_ptr<GpuAsyncContext>)`

### 11.4 等待语义

`GpuTask` 的等待与完成判断必须是**精确完成点**语义。

也就是说：

- `IsCompleted()` 必须针对该 task 自己的目标完成点判断
- `Wait()` 必须只保证该 task 的目标完成点完成
- 不允许退化成“等待共享 fence 的最新值”
- 如果一个 `GpuTask` 内部包含多个 completion point，则上述语义对全部 completion point 同时成立

### 11.5 依赖语义

`GpuTask` 可以被：

- CPU 侧轮询 `IsCompleted()`
- CPU 侧阻塞 `Wait()`
- 其他 `GpuFrameContext` / `GpuAsyncContext` 通过 `WaitFor()` 建立 GPU 侧依赖

### 11.6 生命周期语义

`GpuTask` 必须强持有完成点所需的同步状态。

固定要求：

- 只存裸 `Fence*` 不够
- 依赖某个 `GpuTask` 的上下文必须确保同步对象在提交前不会被提前析构

这条规则用于避免悬空 fence / completion state。

### 11.7 一次 `Submit()` 与一个 `GpuTask`

固定外部语义：

- 一次 `Submit()` 对外只返回一个 `GpuTask`

即使 runtime 内部因为多队列或多 surface 需要拆成多次 backend submit，外部也必须看到一个统一完成语义。

### 11.8 `GpuCompletionState`

为实现多队列统一完成语义，引入 runtime 内部抽象：

```cpp
struct GpuCompletionPoint {
    shared_ptr<render::Fence> Fence;
    uint64_t TargetValue{0};
};

class GpuCompletionState {
public:
    vector<GpuCompletionPoint> Points;
};
```

固定语义：

- 单队列提交时，`Points.size() == 1`
- 多队列提交时，`Points.size() >= 2`
- `GpuTask::IsCompleted()` 表示 `Points` 中所有 completion point 均已完成
- `GpuTask::Wait()` 表示等待 `Points` 中所有 completion point 到达目标值

---

## 12. 资源语义

新 API 固定区分三类资源：

### 12.1 Runtime 持久资源

由 runtime 或资源系统长期持有，可跨帧、跨 task 使用。

典型例子：

- 长寿命 buffer
- 长寿命 texture
- history resource

### 12.2 Frame transient 资源

只属于某个 `GpuFrameContext` / Render Graph。

典型例子：

- 本帧中间 render target
- 本帧临时 compute 输出
- 本帧后处理链路中的临时纹理

### 12.3 Surface back buffer

通过 `GpuSurfaceLease` 导入，生命周期独立于普通资源。

### 12.4 固定规则

- `GpuAsyncContext` 不能使用 frame transient 资源
- `GpuSurfaceLease` 不能被视为普通持久资源
- `GpuTask` 不自动等同于某块 surface 的 present 完成
- surface flight slot 的退休状态必须记录 completion state，而不是只记录一个裸 value

---

## 13. 同步模型

### 13.1 CPU 等待

通过 `GpuTask::Wait()` 或 `GpuTask::IsCompleted()`。

### 13.2 GPU 侧依赖

通过：

- `GpuFrameContext::WaitFor(const GpuTask&)`
- `GpuAsyncContext::WaitFor(const GpuTask&)`

### 13.3 surface pacing

surface 的 flight frame / image acquire 只表达：

- 何时可以安全获取并渲染某个 back buffer

它不等价于：

- 整个 frame submit 的统一完成 fence

### 13.4 多 surface 同步

多 surface 场景下：

- 每块 surface 自己管理 acquire / present pacing
- frame 提交的统一 `GpuTask` 由 runtime 独立提供
- 不允许“只 signal 第一块 surface 的 fence，然后顺便更新所有 surface 状态”

### 13.5 flight slot 退休

surface 的每个 flight slot 必须记录“上一次使用该 slot 的完成状态”。

固定要求：

- 不能只记录一个 `uint64_t`
- 必须记录可被精确查询的 completion state

推荐模型：

```cpp
vector<shared_ptr<GpuCompletionState>> SlotRetireStates;
```

每次某个 slot 被新的 frame submit 使用后：

- 该 slot 绑定到这次 frame 对应的 completion state
- 下一次复用该 slot 前，必须确认该 completion state 已完成

### 13.6 空帧快速路径

`GpuFrameContext` 允许出现“空帧”：

- 没有 acquire 任何 surface
- 没有任何命令
- 没有任何需要提交的 GPU 工作

固定语义：

- 未提交直接析构是合法且推荐的快速路径
- runtime 不应要求空帧也必须 `Submit()`
- `Submit()` 面对空帧时可以返回无效 `GpuTask`，但推荐调用方直接丢弃该 frame context

这条规则适用于：

- 窗口最小化
- 本帧完全无 GPU 工作
- 仅进行了 CPU 侧逻辑但未生成任何 GPU 执行内容

---

## 14. render 层前置要求

新的 runtime 设计依赖 render 层补齐两个正式能力：

### 14.1 `Fence::Wait(targetValue)`

如果 runtime 使用共享 fence + 递增 value 模型，render 层 fence 必须支持精确等待目标值。

固定要求：

```cpp
class Fence : public RenderBase {
public:
    virtual uint64_t GetCompletedValue() const noexcept = 0;
    virtual void Wait(uint64_t targetValue) noexcept = 0;
};
```

仅有无参 `Wait()` 不够，因为它无法精确定义“等待哪个完成点”。

### 14.2 `SwapChain::AcquireNext()` 必须返回状态

render 层必须能够把 acquire 的细粒度状态传递给 runtime。

推荐模型：

```cpp
enum class SwapChainAcquireStatus : uint8_t {
    Success,
    Unavailable,
    Suboptimal,
    OutOfDate,
    Lost,
};

struct AcquireResult {
    SwapChainAcquireStatus Status{SwapChainAcquireStatus::Lost};
    Nullable<Texture*> BackBuffer{nullptr};
    SwapChainSyncObject* WaitToDraw{nullptr};
    SwapChainSyncObject* ReadyToPresent{nullptr};
};
```

否则 runtime 层的 `GpuSurfaceAcquireStatus` 无法真正实现。

### 14.3 `Present()` 状态

理想情况下，render 层 `Present()` 也应返回状态，以便 runtime 能传播：

- `Suboptimal`
- `OutOfDate`
- `Lost`

如果第一阶段暂时不补 `Present()` 状态，至少 `AcquireNext()` 的状态必须先补齐。

---

## 15. 多 swap chain 固定语义

### 15.1 一帧内多个 surface

一帧内允许获取多块 surface：

- 主窗口
- ImGui 主 dock space
- 分离出来的 viewport 窗口
- 工具窗口

### 15.2 独立获取与独立 present

每块 surface 都必须：

- 独立 acquire
- 独立处理 `Suboptimal` / `OutOfDate` / `Lost`
- 独立决定是否 present

### 15.3 推荐策略

- 主窗口通常使用 `AcquireSurface()`
- ImGui 子窗口通常使用 `TryAcquireSurface()`
- 不能获取某个子窗口时，不应阻塞整个主帧

### 15.4 典型时序

```cpp
auto frame = runtime.BeginFrame();

auto mainResult = frame->AcquireSurface(mainSurface);
auto imguiA = frame->TryAcquireSurface(imguiSurfaceA);
auto imguiB = frame->TryAcquireSurface(imguiSurfaceB);

// Render Graph 根据拿到的 lease 决定导入哪些 present target

GpuTask task = runtime.Submit(std::move(frame));
```

---

## 16. 帧与异步提交并存的固定语义

### 16.1 两类 GPU 工作

系统固定区分：

#### A. 帧内工作

属于 `GpuFrameContext`：

- graphics
- frame-local async compute
- present 相关工作

#### B. 脱离帧工作

属于 `GpuAsyncContext`：

- 脱离帧的异步 GPU 工作
- 可包含 detached graphics / compute / copy
- 不依赖 surface
- 不依赖本帧 present 节奏

### 16.2 结果衔接

脱离帧 async work 的结果通过 `GpuTask` 与未来帧衔接：

```cpp
auto asyncContext = runtime.BeginAsync();
GpuTask prepTask = runtime.Submit(std::move(asyncContext));

auto frame = runtime.BeginFrame();
frame->WaitFor(prepTask);
GpuTask frameTask = runtime.Submit(std::move(frame));
```

### 16.3 为什么不把所有 async work 都塞进 frame

因为很多 GPU 工作并不是“某帧的一部分”，例如：

- 后台预计算
- 编辑器工具计算
- 长时间运行的缓存生成
- 离屏缩略图或后台预渲染

这些工作如果被强行绑定到 frame，会把 frame 语义和 detached work 语义混淆。

---

## 17. 建议 API 草案

以下草案只用于固定语义，不代表最终代码细节。

```cpp
enum class GpuSurfaceAcquireStatus : uint8_t {
    Success,
    Unavailable,
    Suboptimal,
    OutOfDate,
    Lost,
};

class GpuTask {
public:
    bool IsValid() const noexcept;
    bool IsCompleted() const noexcept;
    void Wait() noexcept;
};

class GpuPresentSurface {
public:
    bool IsValid() const noexcept;
    bool Reconfigure(
        uint32_t width,
        uint32_t height,
        std::optional<render::TextureFormat> format = std::nullopt,
        std::optional<render::PresentMode> presentMode = std::nullopt) noexcept;
    void Destroy() noexcept;
};

class GpuSurfaceLease {
public:
    bool IsValid() const noexcept;
};

struct GpuSurfaceAcquireResult {
    GpuSurfaceAcquireStatus Status{GpuSurfaceAcquireStatus::Lost};
    Nullable<GpuSurfaceLease*> Lease{nullptr};
};

struct GpuQueueSubmitStep {
    render::QueueType Queue{render::QueueType::Direct};
    vector<uint32_t> WaitStepIndices;
    vector<unique_ptr<render::CommandBuffer>> CommandBuffers;
};

class GpuFrameContext {
public:
    GpuSurfaceAcquireResult AcquireSurface(GpuPresentSurface& surface) noexcept;
    GpuSurfaceAcquireResult TryAcquireSurface(GpuPresentSurface& surface) noexcept;

    bool Present(GpuSurfaceLease& lease) noexcept;
    bool WaitFor(const GpuTask& task) noexcept;
    bool AddGraphicsCommandBuffer(unique_ptr<render::CommandBuffer> cmd) noexcept;
    bool AddComputeCommandBuffer(unique_ptr<render::CommandBuffer> cmd) noexcept;
    bool AddCopyCommandBuffer(unique_ptr<render::CommandBuffer> cmd) noexcept;
    bool SetSubmissionPlan(vector<GpuQueueSubmitStep> steps) noexcept;
    bool IsEmpty() const noexcept;
    Nullable<unique_ptr<render::CommandBuffer>> CreateGraphicsCommandBuffer() noexcept;
    Nullable<unique_ptr<render::CommandBuffer>> CreateComputeCommandBuffer() noexcept;
    Nullable<unique_ptr<render::CommandBuffer>> CreateCopyCommandBuffer() noexcept;
};

class GpuAsyncContext {
public:
    bool WaitFor(const GpuTask& task) noexcept;
    bool AddGraphicsCommandBuffer(unique_ptr<render::CommandBuffer> cmd) noexcept;
    bool AddComputeCommandBuffer(unique_ptr<render::CommandBuffer> cmd) noexcept;
    bool AddCopyCommandBuffer(unique_ptr<render::CommandBuffer> cmd) noexcept;
    bool SetSubmissionPlan(vector<GpuQueueSubmitStep> steps) noexcept;
    bool IsEmpty() const noexcept;
    Nullable<unique_ptr<render::CommandBuffer>> CreateGraphicsCommandBuffer() noexcept;
    Nullable<unique_ptr<render::CommandBuffer>> CreateComputeCommandBuffer() noexcept;
    Nullable<unique_ptr<render::CommandBuffer>> CreateCopyCommandBuffer() noexcept;
};

class GpuRuntime {
public:
    static Nullable<unique_ptr<GpuRuntime>> Create(const GpuRuntimeDescriptor& desc) noexcept;

    Nullable<unique_ptr<GpuPresentSurface>> CreatePresentSurface(
        const GpuPresentSurfaceDescriptor& desc) noexcept;

    Nullable<unique_ptr<GpuFrameContext>> BeginFrame() noexcept;
    Nullable<unique_ptr<GpuAsyncContext>> BeginAsync() noexcept;

    GpuTask Submit(unique_ptr<GpuFrameContext> frame) noexcept;
    GpuTask Submit(unique_ptr<GpuAsyncContext> asyncContext) noexcept;

    void ProcessTasks() noexcept;
    void Destroy() noexcept;
};
```

---

## 18. 当前 API 到新 API 的迁移方向

### 18.1 `GpuSubmitContext`

现有 `GpuSubmitContext` 退役。

拆分为：

- `GpuFrameContext`
- `GpuAsyncContext`

### 18.2 `Acquire(surface) -> BackBufferHandle`

现有 acquire 结果退役。

改为：

- `AcquireSurface() -> GpuSurfaceLease`
- `TryAcquireSurface() -> GpuSurfaceLease`

### 18.3 `Present(surface, backBufferHandle)`

现有 present 方式退役。

改为：

- `Present(GpuSurfaceLease&)`，只标记，不转移所有权

### 18.4 `GpuTask`

名称保留，但语义固定为：

- 完成令牌
- 精确完成点
- 可等待、可查询、可依赖

---

## 19. 固定禁止事项

后续实现中，以下行为一律视为违反设计：

- 用某块 surface 的 fence 充当整次 frame submit 的完成点
- 多 surface 提交时只 signal 第一块 surface 的完成状态
- `GpuTask::Wait()` 等待共享 fence 的“最新值”而不是 task 自己的目标完成点
- `WaitFor()` 只保存裸同步指针，不保存同步对象生命周期
- `AcquireSurface()` 把 lease 所有权交给调用方，导致 frame context 无法在 submit 时访问它
- surface flight slot 只记录裸 value，不记录 completion state
- `GpuAsyncContext` 获取 surface 或使用 frame transient 资源
- `GpuSurfaceLease` 跨帧保存或跨上下文传递
- 把 runtime 变成高层渲染 feature API

---

## 20. 结论

新的 runtime API 固定为以下模型：

- `GpuRuntime`：设备级执行运行时与跨系统 GPU 边界
- `GpuPresentSurface`：长期存活的 swap chain / present surface
- `GpuFrameContext`：为 frame/present/surface 域特化的一帧主上下文
- `GpuAsyncContext`：脱离帧的异步 GPU 上下文
- `GpuSurfaceLease`：frame-local 的 surface acquire / present 权利对象
- `GpuTask`：一次已提交 GPU 工作的完成令牌

这套模型解决的核心问题是：

- 帧主导渲染如何成立
- 脱离帧的异步 GPU 工作如何不被帧语义污染
- 多 swap chain / ImGui 多窗口如何成为一等公民
- Render Graph 如何拥有稳定的执行宿主
- 渲染系统如何拥有对其他系统稳定、可扩展的 GPU 交互边界

后续代码实现应围绕这套固定语义演进，而不再继续扩展当前 `GpuSubmitContext + SurfaceFence` 风格的旧模型。
