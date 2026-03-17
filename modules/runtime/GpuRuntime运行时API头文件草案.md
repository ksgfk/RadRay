# GpuRuntime 运行时 API 头文件草案

## 1. 说明

本文档是**设计草案**，用于展示新的 runtime API 头文件形态。

约束：

- 不修改当前 [gpu_system.h](C:/Users/xiaoxs/Desktop/RadRay/modules/runtime/include/radray/runtime/gpu_system.h)
- 不要求当前实现已经支持本文档中的所有能力
- 仅用于审查命名、分层、生命周期和固定语义

本文档默认基于 [GpuRuntime运行时API重设计.md](C:/Users/xiaoxs/Desktop/RadRay/modules/runtime/GpuRuntime运行时API重设计.md) 的语义结论。

---

## 2. 设计目标

这份头文件草案重点表达以下四件事：

- `GpuRuntime` 是执行运行时与跨系统 GPU 边界
- `GpuFrameContext` 是为 frame/present/surface 域特化的主上下文
- `GpuAsyncContext` 是脱离帧的异步 GPU 上下文
- `GpuTask` 是提交后的完成令牌

同时确保以下场景有正式落点：

- 多 swap chain / ImGui 多 viewport
- future Render Graph 执行宿主
- 帧内 graphics + frame-local async compute
- 脱离帧的后台异步 GPU 工作
- 帧内/上下文内多队列提交顺序由显式 submission plan 表达
- 空帧与空上下文可以走轻量快速路径

---

## 3. 草案头文件

```cpp
#pragma once

#include <optional>

#include <radray/types.h>
#include <radray/nullable.h>
#include <radray/render/common.h>

namespace radray {

class GpuRuntime;
class GpuTask;
class GpuPresentSurface;
class GpuSurfaceLease;
class GpuFrameContext;
class GpuAsyncContext;
class GpuCompletionState;

// ---------------------------------------------------------------------------
// Runtime descriptor
// ---------------------------------------------------------------------------
struct GpuRuntimeDescriptor {
    render::RenderBackend Backend{render::RenderBackend::D3D12};
    bool EnableDebugValidation{false};
    render::RenderLogCallback LogCallback{nullptr};
    void* LogUserData{nullptr};
};

// ---------------------------------------------------------------------------
// Present surface descriptor
// ---------------------------------------------------------------------------
struct GpuPresentSurfaceDescriptor {
    const void* NativeWindowHandle{nullptr};
    uint32_t Width{0};
    uint32_t Height{0};
    uint32_t BackBufferCount{3};
    uint32_t FlightFrameCount{2};
    render::TextureFormat Format{render::TextureFormat::BGRA8_UNORM};
    render::PresentMode PresentMode{render::PresentMode::FIFO};
};

// ---------------------------------------------------------------------------
// GpuTask
// 已提交 GPU 工作的完成令牌
// ---------------------------------------------------------------------------
class GpuTask {
public:
    GpuTask() noexcept = default;
    GpuTask(const GpuTask&) = default;
    GpuTask& operator=(const GpuTask&) = default;
    GpuTask(GpuTask&&) noexcept = default;
    GpuTask& operator=(GpuTask&&) noexcept = default;
    ~GpuTask() noexcept = default;

    bool IsValid() const noexcept;
    bool IsCompleted() const noexcept;
    void Wait() noexcept;

private:
    GpuRuntime* _runtime{nullptr};
    shared_ptr<GpuCompletionState> _completion;

    friend class GpuRuntime;
    friend class GpuFrameContext;
    friend class GpuAsyncContext;
};

// ---------------------------------------------------------------------------
// Surface acquire status
// ---------------------------------------------------------------------------
enum class GpuSurfaceAcquireStatus : uint8_t {
    Success,      // 成功获得可绘制 back buffer
    Unavailable,  // 非阻塞获取失败，但 surface 仍有效
    Suboptimal,   // 可继续使用，但应尽快重建
    OutOfDate,    // 必须重建后才能继续
    Lost,         // surface 已失效
};

// ---------------------------------------------------------------------------
// GpuSurfaceLease
// 一次 frame-local 的 surface 使用权
// ---------------------------------------------------------------------------
class GpuSurfaceLease {
public:
    bool IsValid() const noexcept;

    GpuPresentSurface* GetSurface() const noexcept;
    uint32_t GetFrameSlotIndex() const noexcept;
    render::Texture* GetBackBufferTexture() const noexcept;

private:
    GpuPresentSurface* _surface{nullptr};
    render::Texture* _backBuffer{nullptr};
    render::SwapChainSyncObject* _waitToDraw{nullptr};
    render::SwapChainSyncObject* _readyToPresent{nullptr};
    uint32_t _frameSlotIndex{0};
    bool _presentRequested{false};

    friend class GpuRuntime;
    friend class GpuFrameContext;
};

struct GpuSurfaceAcquireResult {
    GpuSurfaceAcquireStatus Status{GpuSurfaceAcquireStatus::Lost};
    Nullable<GpuSurfaceLease*> Lease{nullptr};
};

// ---------------------------------------------------------------------------
// Physical submission plan
// 由 frame compiler / async work compiler 生成的物理提交步骤
// ---------------------------------------------------------------------------
struct GpuQueueSubmitStep {
    render::QueueType Queue{render::QueueType::Direct};

    // 仅引用同一 submission plan 中更早的 step。
    vector<uint32_t> WaitStepIndices;
    vector<unique_ptr<render::CommandBuffer>> CommandBuffers;
};

// ---------------------------------------------------------------------------
// GpuPresentSurface
// 长期存活的 swap chain / present surface
// ---------------------------------------------------------------------------
class GpuPresentSurface {
public:
    ~GpuPresentSurface() noexcept;

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

private:
    GpuRuntime* _runtime{nullptr};
    unique_ptr<render::SwapChain> _swapchain;

    // surface 自己的 present pacing / frame slot 状态
    uint64_t _frameNumber{0};
    vector<shared_ptr<GpuCompletionState>> _slotRetireStates;

    friend class GpuRuntime;
    friend class GpuFrameContext;
    friend class GpuSurfaceLease;
};

// ---------------------------------------------------------------------------
// GpuFrameContext
// 为 frame / present / surface 域特化的一帧主上下文
// ---------------------------------------------------------------------------
class GpuFrameContext {
public:
    ~GpuFrameContext() noexcept = default;

    GpuSurfaceAcquireResult AcquireSurface(GpuPresentSurface& surface) noexcept;
    GpuSurfaceAcquireResult TryAcquireSurface(GpuPresentSurface& surface) noexcept;

    bool Present(GpuSurfaceLease& lease) noexcept;
    bool WaitFor(const GpuTask& task) noexcept;

    // 单队列简化路径。
    bool AddGraphicsCommandBuffer(unique_ptr<render::CommandBuffer> cmd) noexcept;
    bool AddComputeCommandBuffer(unique_ptr<render::CommandBuffer> cmd) noexcept;
    bool AddCopyCommandBuffer(unique_ptr<render::CommandBuffer> cmd) noexcept;

    // 显式物理提交计划路径。
    bool SetSubmissionPlan(vector<GpuQueueSubmitStep> steps) noexcept;
    bool IsEmpty() const noexcept;

    // 供 Render Graph / runtime 内部执行层使用。
    Nullable<unique_ptr<render::CommandBuffer>> CreateGraphicsCommandBuffer() noexcept;
    Nullable<unique_ptr<render::CommandBuffer>> CreateComputeCommandBuffer() noexcept;
    Nullable<unique_ptr<render::CommandBuffer>> CreateCopyCommandBuffer() noexcept;

private:
    GpuRuntime* _runtime{nullptr};
    vector<unique_ptr<GpuSurfaceLease>> _surfaceLeases;
    vector<shared_ptr<GpuCompletionState>> _dependencies;

    // 仅用于未显式设置 submission plan 时的单队列简化路径。
    vector<unique_ptr<render::CommandBuffer>> _defaultGraphicsCmds;
    vector<unique_ptr<render::CommandBuffer>> _defaultComputeCmds;
    vector<unique_ptr<render::CommandBuffer>> _defaultCopyCmds;

    vector<GpuQueueSubmitStep> _submissionPlan;
    bool _consumed{false};

    friend class GpuRuntime;
};

// ---------------------------------------------------------------------------
// GpuAsyncContext
// 脱离帧的异步 GPU 上下文
// ---------------------------------------------------------------------------
class GpuAsyncContext {
public:
    ~GpuAsyncContext() noexcept = default;

    bool WaitFor(const GpuTask& task) noexcept;

    // 单队列简化路径。
    bool AddGraphicsCommandBuffer(unique_ptr<render::CommandBuffer> cmd) noexcept;
    bool AddComputeCommandBuffer(unique_ptr<render::CommandBuffer> cmd) noexcept;
    bool AddCopyCommandBuffer(unique_ptr<render::CommandBuffer> cmd) noexcept;

    // 显式物理提交计划路径。
    bool SetSubmissionPlan(vector<GpuQueueSubmitStep> steps) noexcept;
    bool IsEmpty() const noexcept;

    // detached async context 允许 graphics / compute / copy，但不允许 acquire/present surface。
    Nullable<unique_ptr<render::CommandBuffer>> CreateGraphicsCommandBuffer() noexcept;
    Nullable<unique_ptr<render::CommandBuffer>> CreateComputeCommandBuffer() noexcept;
    Nullable<unique_ptr<render::CommandBuffer>> CreateCopyCommandBuffer() noexcept;

private:
    GpuRuntime* _runtime{nullptr};
    vector<shared_ptr<GpuCompletionState>> _dependencies;

    // 仅用于未显式设置 submission plan 时的单队列简化路径。
    vector<unique_ptr<render::CommandBuffer>> _defaultGraphicsCmds;
    vector<unique_ptr<render::CommandBuffer>> _defaultComputeCmds;
    vector<unique_ptr<render::CommandBuffer>> _defaultCopyCmds;

    vector<GpuQueueSubmitStep> _submissionPlan;
    bool _consumed{false};

    friend class GpuRuntime;
};

// ---------------------------------------------------------------------------
// GpuRuntime
// 设备级执行运行时与跨系统 GPU 边界
// ---------------------------------------------------------------------------
class GpuRuntime {
public:
    ~GpuRuntime() noexcept;

    static Nullable<unique_ptr<GpuRuntime>> Create(const GpuRuntimeDescriptor& desc) noexcept;

    bool IsValid() const noexcept;
    void Destroy() noexcept;

    Nullable<unique_ptr<GpuPresentSurface>> CreatePresentSurface(
        const GpuPresentSurfaceDescriptor& desc) noexcept;

    Nullable<unique_ptr<GpuFrameContext>> BeginFrame() noexcept;
    Nullable<unique_ptr<GpuAsyncContext>> BeginAsync() noexcept;

    // 对空 frame / async context 允许返回无效 GpuTask；
    // 更推荐调用方在 IsEmpty() 为 true 时直接丢弃上下文而不提交。
    GpuTask Submit(unique_ptr<GpuFrameContext> frame) noexcept;
    GpuTask Submit(unique_ptr<GpuAsyncContext> asyncContext) noexcept;

    void ProcessTasks() noexcept;

private:
    struct InFlightTask {
        shared_ptr<GpuCompletionState> Completion;
        unique_ptr<GpuFrameContext> Frame;
        unique_ptr<GpuAsyncContext> Async;
    };

    render::RenderBackend _backend{render::RenderBackend::D3D12};
    shared_ptr<render::Device> _device;
    unique_ptr<render::InstanceVulkan> _vkInstance;

    // runtime 内部可使用多队列、多 fence、多 target value 组织一次提交，
    // 但对外统一压缩成一个 GpuTask / GpuCompletionState。
    shared_ptr<render::Fence> _graphicsFence;
    shared_ptr<render::Fence> _computeFence;
    shared_ptr<render::Fence> _copyFence;
    uint64_t _graphicsNextValue{1};
    uint64_t _computeNextValue{1};
    uint64_t _copyNextValue{1};

    vector<InFlightTask> _inFlightTasks;

    friend class GpuTask;
    friend class GpuPresentSurface;
    friend class GpuSurfaceLease;
    friend class GpuFrameContext;
    friend class GpuAsyncContext;
};

}  // namespace radray
```

---

## 4. 设计说明

### 4.1 为什么 `GpuTask` 继续保留

保留 `GpuTask` 的原因是它已经很好地表达了：

- 已提交
- 异步完成
- 可等待/可查询/可依赖

在引入 `GpuFrameContext` 和 `GpuAsyncContext` 之后，歧义已经足够小，不需要强行改名。

### 4.1.1 `GpuTask` 内部为什么改成 `GpuCompletionState`

`GpuTask` 对外仍是一个完成令牌，但内部不能再只存单个 `(Fence, Value)`。

原因是：

- 一个 frame 可能同时提交到 graphics / compute / copy 队列
- 一个 detached async 任务本身也可能跨多个队列协作

因此草案固定为：

- `GpuTask` 只持有 `shared_ptr<GpuCompletionState>`
- `GpuCompletionState` 内部可以包含一个或多个精确 completion point
- `GpuTask::IsCompleted()` / `Wait()` 的语义是“所有 completion point 都完成”

### 4.2 为什么引入 `GpuSurfaceLease`

它解决的是当前 API 的一个核心歧义：

- “拿到 back buffer 资源”并不等于“拿到 present 权利”

`GpuSurfaceLease` 把 surface acquire 结果从普通 `GpuResourceHandle` 中剥离出来，单独作为 frame-local 权利对象。

### 4.2.1 为什么 `AcquireSurface()` 改成返回借用指针

lease 的所有权固定属于 `GpuFrameContext`。

因此：

- `AcquireSurface()` / `TryAcquireSurface()` 返回 `GpuSurfaceLease*`
- 调用方只借用它
- `Present(GpuSurfaceLease&)` 只做标记，不消费所有权

这样 frame context 才能在 `Submit()` 时稳定遍历所有已获取的 lease。

### 4.3 为什么 `GpuFrameContext` 和 `GpuAsyncContext` 分开

这是为了把两类工作彻底区分：

- `GpuFrameContext`：属于一帧、围绕 frame/present/surface 语义组织、可能持有 0..N 个 surface
- `GpuAsyncContext`：脱离帧、绝不碰 surface、但可以合法承载 detached graphics / compute / copy work

这样 future Render Graph 也更容易收口：

- frame graph 编译到 `GpuFrameContext`
- detached async graph / job 编译到 `GpuAsyncContext`

### 4.4 为什么 `GpuRuntime` 要有 runtime 级 fence

因为 `GpuTask` 必须表达统一完成点。

如果继续借用 surface fence：

- 多 surface 提交会失真
- 无 surface 的 async 提交没有合法完成点
- `GpuTask::Wait()` 会退化成不精确等待

所以草案里明确让 `GpuRuntime` 自己持有 runtime 级同步来源。

但修正版草案进一步固定：

- runtime 级 fence 是内部同步来源，不直接等同于外部 `GpuTask`
- 外部 `GpuTask` 通过 `GpuCompletionState` 表示统一完成语义
- surface flight slot 退休状态也记录 `GpuCompletionState`，而不是裸 `uint64_t`

### 4.5 为什么 `Create*CommandBuffer()` 暂时仍出现在 context 中

这是草案里刻意保留的“执行层接口”。

原因是：

- runtime 仍然是 Render Graph 的执行宿主
- graph compiler 最终仍需要落到 command buffer 录制

但它们属于执行层接口，不应该成为高层 renderer feature 的长期主入口。

### 4.5.1 为什么还要显式 `SetSubmissionPlan()`

只创建命令缓冲而不显式表达物理提交顺序，不足以表达真实执行关系。

例如：

- compute 先写某个资源
- direct/graphics 再读这个资源
- copy 最后回收或导出结果

这类顺序如果没有显式物理计划，runtime 就只能“猜”提交顺序，而这是不可靠的。

因此草案固定为：

- compiler 负责生成 `GpuQueueSubmitStep`
- step 明确指定队列、命令和 step 间等待关系
- `GpuRuntime::Submit(...)` 按 plan 执行，而不是从“命令属于哪个队列类别”反推依赖

### 4.5.2 为什么还要保留 `Add*CommandBuffer()`

只保留 `SetSubmissionPlan()` 会让 Render Graph compiler 很舒服，但会让简单场景过重：

- 单队列测试
- 早期 demo
- 临时工具程序
- 还没有 graph compiler 的开发阶段

因此草案固定提供一条**单队列简化路径**：

- `Create*CommandBuffer()` 创建命令
- 调用方录制命令
- 通过对应的 `AddGraphicsCommandBuffer()` / `AddComputeCommandBuffer()` / `AddCopyCommandBuffer()` 把命令交回 context
- `Submit()` 在内部自动把它们收敛成一个单 step submission plan

这条路径的固定约束是：

- 它只适用于单队列默认语义
- 同一个 context 中，`Add*CommandBuffer()` 与 `SetSubmissionPlan()` 互斥
- 如果已经通过 `Add*CommandBuffer()` 收集了命令，再调用 `SetSubmissionPlan()` 必须失败
- 如果已经设置了 `SetSubmissionPlan()`，再调用 `Add*CommandBuffer()` 也必须失败
- 单队列简化路径下，只允许实际使用一个队列类型；如果需要多队列或跨队列依赖，必须切换到显式 `SetSubmissionPlan()`

也就是说，`Create*CommandBuffer()` 拿到的命令缓冲必须最终进入以下两条路径之一：

- 被移入 `GpuQueueSubmitStep`，然后交给 `SetSubmissionPlan()`
- 被移入对应的 `Add*CommandBuffer()`

不能出现“创建了命令但没有被任何提交路径接管”的模糊状态。

### 4.6 为什么改成 `GpuAsyncContext`

既然 detached work 已经明确需要支持 graphics，继续沿用 `GpuComputeContext` 会让名字和能力边界长期冲突。

因此草案改为：

- `GpuAsyncContext` 表示“脱离帧的异步 GPU 工作”
- 它允许 graphics / compute / copy
- 但仍然明确禁止 surface acquire / present 与 frame-transient 资源

这样收口后的分层标准变成：

- 是否属于 frame/present 域，决定用 `GpuFrameContext`
- 是否脱离帧独立提交，决定用 `GpuAsyncContext`

### 4.7 空帧与空上下文快速路径

草案现在明确支持“空 frame / 空 async context”：

- 没有命令
- 没有 surface lease
- 没有需要提交的 GPU 工作

推荐路径是：

- 调用方通过 `IsEmpty()` 判断后直接丢弃上下文
- 不要求空上下文也必须 `Submit()`

如果调用方仍然提交空上下文：

- `Submit()` 可以返回无效 `GpuTask`
- runtime 不必为了空提交构造完整 in-flight 记录

---

## 5. 配套的 render 层前置改动

这份 runtime 头文件草案依赖 render 层补齐两个正式能力。

### 5.1 `Fence::Wait(targetValue)`

```cpp
class Fence : public RenderBase {
public:
    virtual uint64_t GetCompletedValue() const noexcept = 0;
    virtual void Wait(uint64_t targetValue) noexcept = 0;
};
```

如果没有目标值等待，`GpuTask::Wait()` 就无法保证精确完成点语义。

### 5.2 `SwapChain::AcquireNext()` 状态化

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

否则 runtime 层无法真正实现 `GpuSurfaceAcquireStatus`。

---

## 6. 建议审查点

看这份草案时，建议重点看这几件事：

- `GpuFrameContext` / `GpuAsyncContext` 的边界是否足够清晰
- `GpuSurfaceLease` 的“frame context 持有、调用方借用”模型是否更稳
- `GpuTask` 继续保留是否还能维持语义清楚
- `GpuTask + GpuCompletionState` 是否足以表达多队列统一完成点
- `Add*CommandBuffer()` 与 `SetSubmissionPlan()` 的互斥规则是否足够清晰
- `GpuQueueSubmitStep + SetSubmissionPlan()` 是否足以承接 graph compiler 产出的物理提交顺序
- `GpuRuntime` 作为“graph 执行基础设施 + 外部边界”是否合适

---

## 7. 暂不展开的问题

这份草案还没有继续细化以下问题：

- Render Graph 编译器如何把执行产物灌入 `GpuFrameContext` / `GpuAsyncContext`
- runtime 如何把 `GpuQueueSubmitStep` 进一步打包成 backend submit 与 fence signal
- copy/upload/readback 是否独立成专门上下文
- persistent runtime resource 的正式 API
- 设备丢失后的恢复流程

这些问题适合在头文件轮廓确认后再继续收敛。
