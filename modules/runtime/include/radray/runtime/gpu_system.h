#pragma once

#include <array>
#include <limits>
#include <mutex>
#include <stdexcept>

#include <radray/types.h>
#include <radray/nullable.h>
#include <radray/render/common.h>

// gpu_system 语义约定
//
// - 本层是 runtime 对外的长期稳定 GPU API 边界，用于和其他模块交互，
//   并定义“GPU 提交批次(batch)”与“帧(frame)”这两个上层概念。
// - 本层不试图完整封装底层 RHI 的命令系统；命令记录可开放高层 GPU 命令，
//   但提交语义由 runtime 统一定义。
// - GpuRuntime::SubmitAsync / SubmitFrame 表示提交一个 batch。一个 batch 对应一次 runtime
//   定义的提交，并以单个 fence/semaphore 完成令牌进行同步；返回的 GpuTask 仅表示该 batch
//   对应的 queue submission 完成，不表示 present 已结束。
// - 与 swapchain/surface 关联的帧命令由 runtime 封装；frame 的 acquire/present 生命周期
//   不直接作为底层 RHI primitive 暴露给外部模块。
// - GpuResourceHandle 故意保持无类型：它表示 runtime 稳定资源标识，并提供到底层对象的
//   interop 穿透能力，但不在该层统一资源具体类型。
//   其中 Handle 是由 GpuRuntime 创建资源时分配的稳定 opaque id，调用方不应自行构造或修改；
//   NativeHandle 是后端对象的 borrowed interop 指针，不转移所有权，也不保证跨后端统一具体类型。
// - runtime 当前不负责 GPU 资源状态/usage 追踪；资源状态/barrier 以及
//   DestroyResourceImmediate 的 last-use 正确性由更上层负责。
// - 资源销毁入口统一为 DestroyResourceImmediate / DestroyResourceAfter，不按资源类型拆分 public API。
//   DestroyResourceImmediate 立即物理销毁资源，要求当前不存在仍会访问它的 in-flight GPU work；
//   对 texture 还要求不存在仍存活的从属 view。DestroyResourceAfter 则把物理释放延迟到
//   指定 GpuTask 完成后由 ProcessTasks() 调度执行，且对 texture 仍受从属 view 生命周期约束。
// - GpuAsyncContext 可暴露临时资源创建入口；其语义是“从属于该 context 的提交生命周期”，
//   而不是简单绑定到 C++ 对象析构时刻。
// - DependsOn 只记录 GPU 侧依赖，不做 CPU 同步；依赖任务必须来自同一个 GpuRuntime。
// - 这里的“异步”仅指 CPU 与 GPU 执行异步；CPU 侧公开 API 的调用语义保持同步。
// - 错误处理以“状态 + 异常”混合方式向上传播：BeginFrame 的非致命 acquire 结果通过
//   BeginFrameResult::Status 返回；SubmitFrame 的 present 结果通过 SubmitFrameResult::Present
//   返回（当前有效状态约束为 Success / RequireRecreate / Error）；真正的 GPU 致命错误仍通过异常报告。
// - release 构建以低开销为第一目标：运行时不额外承担昂贵的一致性/所有权校验成本；
//   跨 runtime、frame one-shot 消费、对象归属等强约束检查主要放在 debug 构建中主动捕获。
//   因此调用方必须遵守本文档里的生命周期与归属约定；违反约定在 release 下属于未定义行为。
// - BeginFrame 当前只报告 RequireRecreate，不在 runtime 内自动重建 surface/swapchain。
// - TryBeginFrame 是非阻塞接口：当 frame slot 或 swapchain 当前不可用时返回 RetryLater。
// - GpuSurfaceDescriptor::QueueSlot / BeginAsync 的 slot 参数表示 queue 槽位(queue slot)。
// - GpuTask 仅表示一次 runtime submission 的 queue/GPU 完成令牌：对普通异步提交表示该 batch
//   已执行完成，对 frame 提交表示该 frame 的命令提交已执行完成，但都不表示 swapchain present
//   已结束；它借用 source runtime/fence 的生命周期。
// - SubmitAsync 接管 GpuAsyncContext 生命周期并提交一个普通异步 batch；返回的 GpuTask 仅表示
//   对应 queue submission 完成。
// - SubmitFrame 提交一个 frame batch 并推进 present；返回的 GpuTask 只表示该 frame 命令提交完成，
//   present 结果单独通过 SubmitFrameResult::Present 表示。
// - AbandonFrame 用于处理空 frame：它会丢弃当前 frame 上已记录但未提交的用户命令，并通过
//   no-op submit + present 合法收口 acquire/present 生命周期；返回值语义与 SubmitFrame 对称。
// - ProcessTasks 只回收 runtime 已知的已完成 pending submission 持有的内部资源与 context，
//   不等待底层 present 生命周期。
// - Wait 是对指定 queue slot 的底层 queue wait 的薄封装，并会在返回前调用一次 ProcessTasks()；
//   它不等价于整个 runtime 空闲，也不替调用方追踪其他 queue slot、未提交 context 或外部 handle 生命周期。
// - 线程安全约定：
//   - 同一个 GpuRuntime 实例的公开成员函数可被多个线程并发调用；实现会串行化 runtime 内部共享状态。
//   - GpuTask::IsValid / IsCompleted / Wait 在其 source runtime 仍存活时可跨线程调用。
//   - GpuSurface 的公开查询和 Destroy 会通过 source runtime 串行化；surface frame lifecycle
//     状态只保证在 GpuRuntime::BeginFrame/TryBeginFrame/SubmitFrame/AbandonFrame 路径内安全推进。
//   - GpuAsyncContext / GpuFrameContext 是单所有者命令构建对象，不提供线程安全保证；transient
//     resource registry 也不额外加锁。
//   - 以 "_" 开头的成员，以及通过 GetDevice()/NativeHandle 等 escape hatch 获得的 borrowed 对象，
//     不纳入本层线程安全保证，仅作为内部/测试 seam 使用。
// - 生命周期约定：
//   - GpuRuntime 是本层所有对象的根所有者。GpuTask / GpuSurface / GpuAsyncContext /
//     GpuFrameContext 以及由它们暴露的 borrowed/native handle 都不得长于 GpuRuntime。
//   - GpuSurface 封装与某个 present queue 绑定的 swapchain 及其 frame slots。
//   - GpuSurface 必须长于从它 Acquire 成功得到的 GpuFrameContext，以及该 frame 对应的
//     runtime 内部 in-flight 提交；在相关提交 drain 完成前销毁或重建该 surface 属于未定义行为。
//   - GpuSurface::Destroy()/析构只是立即释放持有的底层 swapchain，不等待 queue/present，
//     也不回收 runtime pending。
//   - BeginFrame / TryBeginFrame 一旦成功返回 GpuFrameContext，调用方必须且只能将该 context
//     交给同一个 GpuRuntime 消费一次；丢弃、不提交、重复提交、跨 runtime 提交都属于未定义行为。
//   - 对非空 frame（最终存在至少一个可提交命令缓冲）必须调用 SubmitFrame。
//   - 对空 frame（最终没有任何可提交用户命令）必须调用 AbandonFrame；不得把空 frame 交给 SubmitFrame。
//   - AbandonFrame 会丢弃当前 frame 上已记录但尚未提交的用户命令，仅负责把 acquire/present 生命周期合法收口。
//   - Destroy()/析构不是同步点，不会自动等待 queue idle、present 完成或 pending drain。
//   - 在销毁或重建 GpuSurface 前，调用方必须先确保不存在尚未被 SubmitFrame/AbandonFrame 消费的
//     GpuFrameContext，并且该 surface 相关的 in-flight submission/present 已由外部正确收口
//     （例如显式等待相关 queue slot）；否则属于未定义行为。
//   - GpuRuntime::Destroy()/析构会立即释放 runtime 持有的 device/fence/registry/pending 容器，
//     本身不是同步点。
//   - 在销毁 GpuRuntime 前，调用方必须先确保所有派生对象都已停止使用，相关 GpuTask 已完成，
//     并调用 ProcessTasks() 回收 runtime 持有的 pending contexts；否则属于未定义行为。
//   - 通过 GpuAsyncContext 创建的 transient 资源在 context 未提交即析构时可直接失效；
//     若 context 已被 SubmitAsync / SubmitFrame / AbandonFrame 接管，则其生命周期至少持续到
//     对应 pending 提交被 ProcessTasks() drain 完成。当前实现通过 context-local registry
//     与 runtime pending drain 共同保证该语义。
//

namespace radray {

class GpuRuntime;
class GpuTask;
class GpuSurface;
class GpuFrameContext;
class GpuAsyncContext;
class GpuResourceRegistry;
class RenderGraph;

class GpuSystemException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct GpuResourceHandle {
    uint64_t Handle{std::numeric_limits<uint64_t>::max()};

    void* NativeHandle{nullptr};

    constexpr auto IsValid() const { return Handle != std::numeric_limits<uint64_t>::max(); }

    constexpr void Invalidate() {
        Handle = std::numeric_limits<uint64_t>::max();
        NativeHandle = nullptr;
    }

    constexpr static GpuResourceHandle Invalid() { return {std::numeric_limits<uint64_t>::max(), nullptr}; }
};

struct GpuBufferHandle : GpuResourceHandle {};
struct GpuTextureHandle : GpuResourceHandle {};
struct GpuTextureViewHandle : GpuResourceHandle {};
struct GpuSamplerHandle : GpuResourceHandle {};

struct GpuTextureViewDescriptor {
    GpuTextureHandle Target{};
    render::TextureDimension Dim{render::TextureDimension::UNKNOWN};
    render::TextureFormat Format{render::TextureFormat::UNKNOWN};
    render::SubresourceRange Range{};
    render::TextureViewUsages Usage{render::TextureViewUsage::UNKNOWN};
};

struct GpuSurfaceDescriptor {
    const void* NativeHandler{nullptr};
    uint32_t Width{0};
    uint32_t Height{0};
    uint32_t BackBufferCount{0};
    uint32_t FlightFrameCount{1};
    render::TextureFormat Format{render::TextureFormat::UNKNOWN};
    render::PresentMode PresentMode{render::PresentMode::FIFO};
    uint32_t QueueSlot{0};
};

class GpuTask {
public:
    GpuTask(GpuRuntime* runtime, render::Fence* fence, uint64_t signalValue) noexcept;
    GpuTask(const GpuTask&) noexcept = delete;
    GpuTask& operator=(const GpuTask&) noexcept = delete;
    GpuTask(GpuTask&&) noexcept = default;
    GpuTask& operator=(GpuTask&&) noexcept = default;
    ~GpuTask() noexcept = default;

    bool IsValid() const;
    bool IsCompleted() const;
    void Wait();

public:
    GpuRuntime* _runtime{nullptr};
    render::Fence* _fence{nullptr};
    uint64_t _signalValue{0};
};

class GpuSurface {
public:
    GpuSurface(
        GpuRuntime* runtime,
        unique_ptr<render::SwapChain> swapchain,
        uint32_t queueSlot) noexcept;
    ~GpuSurface() noexcept;

    bool IsValid() const;
    void Destroy();

    render::SwapChainDescriptor GetDesc() const;
    uint32_t GetQueueSlot() const;
    size_t GetNextFrameSlotIndex() const;
    uint32_t GetWidth() const;
    uint32_t GetHeight() const;
    render::TextureFormat GetFormat() const;
    render::PresentMode GetPresentMode() const;
    uint32_t GetFlightFrameCount() const;

public:
    class Frame {
    public:
        uint64_t _fenceValue{0};
    };

    GpuRuntime* _runtime{nullptr};
    unique_ptr<render::SwapChain> _swapchain;
    vector<Frame> _frameSlots;
    size_t _nextFrameSlotIndex{0};
    uint32_t _queueSlot{0};
};

class GpuAsyncContext {
public:
    enum class Kind {
        Async,
        Frame
    };

    GpuAsyncContext(
        GpuRuntime* runtime,
        render::CommandQueue* _queue,
        uint32_t queueSlot) noexcept;
    GpuAsyncContext(const GpuAsyncContext&) noexcept = delete;
    GpuAsyncContext& operator=(const GpuAsyncContext&) noexcept = delete;
    GpuAsyncContext(GpuAsyncContext&&) noexcept = delete;
    GpuAsyncContext& operator=(GpuAsyncContext&&) noexcept = delete;
    virtual ~GpuAsyncContext() noexcept;

    bool IsEmpty() const;

    bool DependsOn(const GpuTask& task);
    render::CommandBuffer* CreateCommandBuffer();
    GpuBufferHandle CreateTransientBuffer(const render::BufferDescriptor& desc);
    GpuTextureHandle CreateTransientTexture(const render::TextureDescriptor& desc);
    GpuTextureViewHandle CreateTransientTextureView(const GpuTextureViewDescriptor& desc);

    constexpr Kind GetType() const { return _type; }

public:
    GpuRuntime* _runtime{nullptr};
    render::CommandQueue* _queue{nullptr};
    vector<unique_ptr<render::CommandBuffer>> _cmdBuffers;

public:
    vector<render::Fence*> _waitFences;
    vector<uint64_t> _waitValues;
    uint32_t _queueSlot{0};
    Kind _type{Kind::Async};
    unique_ptr<GpuResourceRegistry> _resourceRegistry;
};

class GpuFrameContext : public GpuAsyncContext {
public:
    GpuFrameContext(
        GpuRuntime* runtime,
        GpuSurface* surface,
        size_t frameSlotIndex,
        render::Texture* backBuffer,
        uint32_t backBufferIndex) noexcept;

    ~GpuFrameContext() noexcept override;

    render::Texture* GetBackBuffer() const;
    uint32_t GetBackBufferIndex() const;

public:
#ifdef RADRAY_IS_DEBUG
    enum class ConsumeState {
        Acquired,
        Submitted,
        Abandoned
    };
#endif

    GpuSurface* _surface{nullptr};
    size_t _frameSlotIndex{std::numeric_limits<size_t>::max()};
    render::Texture* _backBuffer{nullptr};
    Nullable<render::SwapChainSyncObject*> _waitToDraw{nullptr};
    Nullable<render::SwapChainSyncObject*> _readyToPresent{nullptr};
    uint32_t _backBufferIndex{std::numeric_limits<uint32_t>::max()};
    std::optional<render::SwapChainFrame> _acquiredFrame{};
#ifdef RADRAY_IS_DEBUG
    ConsumeState _consumeState{ConsumeState::Acquired};
#endif
};

class GpuRuntime {
public:
    struct BeginFrameResult {
        Nullable<unique_ptr<GpuFrameContext>> Context;
        render::SwapChainStatus Status{render::SwapChainStatus::Error};
    };

    struct SubmitFrameResult {
        GpuTask Task;
        render::SwapChainPresentResult Present;
    };

    GpuRuntime(
        shared_ptr<render::Device> device,
        unique_ptr<render::InstanceVulkan> vkInstance) noexcept;
    GpuRuntime(const GpuRuntime&) noexcept = delete;
    GpuRuntime& operator=(const GpuRuntime&) noexcept = delete;
    GpuRuntime(GpuRuntime&&) noexcept = delete;
    GpuRuntime& operator=(GpuRuntime&&) noexcept = delete;
    ~GpuRuntime() noexcept;

    bool IsValid() const;

    void Destroy() noexcept;

    render::Device* GetDevice() const;

    unique_ptr<GpuSurface> CreateSurface(const GpuSurfaceDescriptor& desc);

    GpuBufferHandle CreateBuffer(const render::BufferDescriptor& desc);

    GpuTextureHandle CreateTexture(const render::TextureDescriptor& desc);

    GpuTextureViewHandle CreateTextureView(const GpuTextureViewDescriptor& desc);

    GpuSamplerHandle CreateSampler(const render::SamplerDescriptor& desc);

    void DestroyResourceImmediate(GpuResourceHandle handle);

    void DestroyResourceAfter(GpuResourceHandle handle, const GpuTask& task);

    BeginFrameResult BeginFrame(GpuSurface* surface);

    BeginFrameResult TryBeginFrame(GpuSurface* surface);

    unique_ptr<GpuAsyncContext> BeginAsync(render::QueueType type, uint32_t queueSlot = 0);

    GpuTask SubmitAsync(unique_ptr<GpuAsyncContext> context);

    SubmitFrameResult SubmitFrame(unique_ptr<GpuFrameContext> context);

    SubmitFrameResult AbandonFrame(unique_ptr<GpuFrameContext> context);

    void ProcessTasks();

    void ProcessTasksUnlocked();

    void Wait(render::QueueType type, uint32_t queueSlot = 0);

    static Nullable<unique_ptr<GpuRuntime>> Create(const render::VulkanDeviceDescriptor& desc, render::VulkanInstanceDescriptor vkInsDesc);

    static Nullable<unique_ptr<GpuRuntime>> Create(const render::D3D12DeviceDescriptor& desc);

public:
    class SubmittedBatch {
    public:
        render::Fence* Fence{nullptr};
        uint64_t SignalValue{0};
    };

    class ResourceRetirement {
    public:
        GpuResourceHandle Handle{};
        render::Fence* Fence{nullptr};
        uint64_t SignalValue{0};

        bool IsReady() const noexcept;
    };

    class Pending {
    public:
        Pending(
            unique_ptr<GpuAsyncContext> context,
            render::Fence* fence,
            uint64_t signalValue) noexcept
            : _context(std::move(context)),
              _fence(fence),
              _signalValue(signalValue) {}
        Pending(const Pending&) noexcept = delete;
        Pending& operator=(const Pending&) noexcept = delete;
        Pending(Pending&&) noexcept = default;
        Pending& operator=(Pending&&) noexcept = default;

    public:
        unique_ptr<GpuAsyncContext> _context;
        render::Fence* _fence{nullptr};
        uint64_t _signalValue{0};
    };

    render::Fence* GetQueueFenceUnlocked(render::QueueType type, uint32_t slot);
    SubmittedBatch SubmitContextUnlocked(GpuAsyncContext* context, Nullable<render::SwapChainSyncObject*> waitToExecute, Nullable<render::SwapChainSyncObject*> readyToPresent);
#ifdef RADRAY_IS_DEBUG
    void ValidateFrameContextForConsume(const char* apiName, const GpuFrameContext* context) const;
#endif
    SubmitFrameResult FinalizeFrameUnlocked(unique_ptr<GpuFrameContext> context);
    GpuRuntime::BeginFrameResult AcquireSwapChainUnlocked(GpuSurface* surface, uint64_t timeoutMs);

    mutable std::mutex _runtimeMutex;
    shared_ptr<render::Device> _device;
    unique_ptr<render::InstanceVulkan> _vkInstance;
    std::array<vector<unique_ptr<render::Fence>>, (size_t)render::QueueType::MAX_COUNT> _queueFences;
    vector<Pending> _pendings;
    unique_ptr<GpuResourceRegistry> _resourceRegistry;
    vector<ResourceRetirement> _resourceRetirements;
};

}  // namespace radray
