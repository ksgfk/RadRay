#pragma once

#include <array>
#include <limits>
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
// - runtime 当前不负责 GPU 资源状态追踪；资源状态/barrier 由更上层负责。
// - 资源销毁入口统一为 DestroyResource / DestroyResourceAfter，不按资源类型拆分 public API。
// - GpuAsyncContext 可暴露临时资源创建入口；其语义是“从属于该 context 的提交生命周期”，
//   而不是简单绑定到 C++ 对象析构时刻。
// - GpuResourceHandle::NativeHandle 保持 void* 作为后端 interop 穿透指针；本层不统一其具体承载类型。
// - 这里的“异步”仅指 CPU 与 GPU 执行异步；CPU 侧公开 API 的调用语义保持同步。
// - 错误处理以“状态 + 异常”混合方式向上传播：BeginFrame 的非致命 acquire 结果通过
//   BeginFrameResult::Status 返回；SubmitFrame 的 present 结果通过 SubmitFrameResult::Present
//   返回（当前有效状态约束为 Success / RequireRecreate / Error）；真正的 GPU 致命错误仍通过异常报告。
// - BeginFrame 当前只报告 RequireRecreate，不在 runtime 内自动重建 surface/swapchain。
// - TryBeginFrame 是非阻塞接口：当 frame slot 或 swapchain 当前不可用时返回 RetryLater。
// - CreateSurface / BeginAsync 的 slot 参数表示 queue 槽位(queue slot)。
// - DependsOn 用于表示当前异步上下文依赖于某个 GPU 任务完成，没有 CPU 同步语义
// - 生命周期约定：
//   - GpuRuntime 是本层所有对象的根所有者。GpuTask / GpuSurface / GpuAsyncContext /
//     GpuFrameContext 以及由它们暴露的 borrowed/native handle 都不得长于 GpuRuntime。
//   - GpuSurface 必须长于从它 Acquire 成功得到的 GpuFrameContext，以及该 frame 对应的
//     runtime 内部 in-flight 提交；在相关提交 drain 完成前销毁或重建该 surface 属于未定义行为。
//   - BeginFrame / TryBeginFrame 一旦成功返回 GpuFrameContext，调用方必须且只能将该 context
//     交给同一个 GpuRuntime::SubmitFrame 或 GpuRuntime::AbandonFrame 一次；丢弃、不提交、重复提交、
//     跨 runtime 提交都属于未定义行为。
//   - AbandonFrame 会丢弃当前 frame 上已记录但尚未提交的用户命令，仅负责把 acquire/present 生命周期合法收口。
//   - 在销毁或重建 GpuSurface / GpuRuntime 前，调用方必须先确保相关 GpuTask 已完成，并调用
//     ProcessTasks() 回收 runtime 持有的 pending contexts，使 frame/context 完成 drain；
//     这不表示底层 present 生命周期已被纳入 GpuTask。
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

class GpuSystemException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct GpuResourceHandle {
    // Runtime 层分配的稳定对象标识。
    // - 由 GpuRuntime 在创建资源时生成
    // - 调用方不应自行构造或修改
    // - 生命周期语义由 runtime 决定；当对象失效后，该 Handle 也随之失效
    uint64_t Handle{std::numeric_limits<uint64_t>::max()};

    // 后端 RHI 层对象的穿透指针。
    // - 该指针不转移所有权，只是一个 borrowed handle
    // - 具体指向什么对象由后端自行定义，例如某个 render 后端里的 Buffer/Texture/CommandBuffer 实现对象
    // - 其生命周期与 Handle 一致；当资源失效后，该指针也随之失效
    // - 调用方不应假设不同后端下它具有统一的具体类型
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

class GpuTask {
public:
    // GpuTask 仅表示一次 runtime submission 的 queue/GPU 完成令牌。
    // - 对普通异步提交：表示该 batch 已执行完成；
    // - 对 frame 提交：表示该 frame 的命令提交已执行完成；
    // - 它不表示 swapchain present 已结束。
    GpuTask(GpuRuntime* runtime, render::Fence* fence, uint64_t signalValue) noexcept;
    GpuTask(const GpuTask&) noexcept = delete;
    GpuTask& operator=(const GpuTask&) noexcept = delete;
    GpuTask(GpuTask&&) noexcept = default;
    GpuTask& operator=(GpuTask&&) noexcept = default;
    ~GpuTask() noexcept = default;

    bool IsValid() const;
    bool IsCompleted() const;
    void Wait();

private:
    GpuRuntime* _runtime{nullptr};
    render::Fence* _fence{nullptr};
    uint64_t _signalValue{0};

    friend class GpuAsyncContext;
    friend class GpuRuntime;
};

class GpuSurface {
public:
    // Surface 封装与某个 present queue 绑定的 swapchain 及其 frame slots。
    // 调用方负责保证：
    // - 它长于所有从该 surface 成功 acquire 出来的 frame；
    // - 在销毁/重建前，相关 in-flight 提交已经完成并被 ProcessTasks() 回收。
    GpuSurface(
        GpuRuntime* runtime,
        unique_ptr<render::SwapChain> swapchain,
        uint32_t queueSlot) noexcept;
    ~GpuSurface() noexcept;

    bool IsValid() const;
    void Destroy();

    uint32_t GetWidth() const;
    uint32_t GetHeight() const;
    render::TextureFormat GetFormat() const;
    render::PresentMode GetPresentMode() const;

private:
    class Frame {
    public:
        uint64_t _fenceValue{0};
    };

    GpuRuntime* _runtime{nullptr};
    unique_ptr<render::SwapChain> _swapchain;
    vector<Frame> _frameSlots;
    size_t _nextFrameSlotIndex{0};
    uint32_t _queueSlot{0};

    friend class GpuRuntime;
    friend class GpuAsyncContext;
    friend class GpuFrameContext;
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

    // 记录 GPU 侧依赖关系，不做 CPU 同步。
    // task 必须来自同一个 GpuRuntime；跨 runtime 依赖没有定义。
    // 调试构建会主动捕获这类误用。
    bool DependsOn(const GpuTask& task);
    render::CommandBuffer* CreateCommandBuffer();
    // transient 资源由当前 context 所有。
    // - 若 context 未提交即析构，可直接失效；
    // - 若 context 已被 runtime 接管提交，则其生命周期至少持续到对应 pending drain 完成。
    GpuBufferHandle CreateTransientBuffer(const render::BufferDescriptor& desc);
    GpuTextureHandle CreateTransientTexture(const render::TextureDescriptor& desc);
    GpuTextureViewHandle CreateTransientTextureView(const GpuTextureViewDescriptor& desc);

    constexpr Kind GetType() const { return _type; }

protected:
    GpuRuntime* _runtime{nullptr};
    render::CommandQueue* _queue{nullptr};
    vector<unique_ptr<render::CommandBuffer>> _cmdBuffers;

private:
    vector<render::Fence*> _waitFences;
    vector<uint64_t> _waitValues;
    uint32_t _queueSlot{0};
    Kind _type{Kind::Async};
    unique_ptr<GpuResourceRegistry> _resourceRegistry;

    friend class GpuRuntime;
};

class GpuFrameContext : public GpuAsyncContext {
public:
    // GpuFrameContext 表示一次成功 acquire 的 frame。
    // - 它借用所属 GpuSurface 的 frame/swapchain 状态；
    // - 调用方必须且只能将其交给同一个 GpuRuntime::SubmitFrame 或 GpuRuntime::AbandonFrame 一次；
    // - AbandonFrame 会丢弃当前 frame 上已记录但尚未提交的用户命令，仅负责退休 acquire/present 生命周期。
    GpuFrameContext(
        GpuRuntime* runtime,
        GpuSurface* surface,
        size_t frameSlotIndex,
        render::Texture* backBuffer,
        uint32_t backBufferIndex) noexcept;

    ~GpuFrameContext() noexcept override;

    render::Texture* GetBackBuffer() const;
    uint32_t GetBackBufferIndex() const;

private:
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
#ifdef RADRAY_IS_DEBUG
    ConsumeState _consumeState{ConsumeState::Acquired};
#endif

    friend class GpuRuntime;
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
    GpuRuntime(GpuRuntime&&) noexcept = default;
    GpuRuntime& operator=(GpuRuntime&&) noexcept = default;
    ~GpuRuntime() noexcept;

    bool IsValid() const;
    void Destroy() noexcept;

    unique_ptr<GpuSurface> CreateSurface(
        const void* nativeHandler,
        uint32_t width, uint32_t height,
        uint32_t backBufferCount,
        uint32_t flightFrameCount,
        render::TextureFormat format,
        render::PresentMode presentMode,
        uint32_t queueSlot = 0);
    GpuBufferHandle CreateBuffer(const render::BufferDescriptor& desc);
    GpuTextureHandle CreateTexture(const render::TextureDescriptor& desc);
    GpuTextureViewHandle CreateTextureView(const GpuTextureViewDescriptor& desc);
    GpuSamplerHandle CreateSampler(const render::SamplerDescriptor& desc);
    // 逻辑上立即销毁 handle，对象物理释放延迟到后续 ProcessTasks() 调度。
    void DestroyResource(GpuResourceHandle handle);
    // 与 DestroyResource 相同，但额外要求对应 task 完成后才允许 ProcessTasks() 真正释放对象。
    void DestroyResourceAfter(GpuResourceHandle handle, const GpuTask& task);

    // 成功时返回 one-shot 的 GpuFrameContext。
    // 该 context 必须且只能被 SubmitFrame 或 AbandonFrame 消费一次；调用方不得丢弃或复用它。
    BeginFrameResult BeginFrame(GpuSurface* surface);

    // 非阻塞版本。若返回 Success，则约束与 BeginFrame 相同。
    BeginFrameResult TryBeginFrame(GpuSurface* surface);

    unique_ptr<GpuAsyncContext> BeginAsync(render::QueueType type, uint32_t queueSlot = 0);

    // 提交一个普通异步 batch，并接管 context 的生命周期。
    // 返回的 GpuTask 仅表示该提交对应的 queue 完成。
    GpuTask SubmitAsync(unique_ptr<GpuAsyncContext> context);

    // 提交一个 frame batch，并继续完成 present 流程。
    // 返回的 GpuTask 仅表示该 frame 的命令提交已执行完成；
    // SubmitFrameResult::Present 单独表示 present 的结果（当前有效状态约束为 Success / RequireRecreate / Error）。
    SubmitFrameResult SubmitFrame(unique_ptr<GpuFrameContext> context);

    // 放弃当前 frame，丢弃用户已记录但尚未提交的 frame 命令，并以 no-op submit + present
    // 合法退休 acquire/present 生命周期。
    // 返回值与 SubmitFrame 对称：Task 表示这次 no-op frame submission 的 queue/GPU 完成，
    // Present 单独表示 present 的结果（当前有效状态约束为 Success / RequireRecreate / Error）。
    SubmitFrameResult AbandonFrame(unique_ptr<GpuFrameContext> context);

    // 回收已完成的 pending 提交持有的内部资源/contexts。
    // 若调用方等待了 GpuTask 并准备销毁或重建相关 surface/runtime，必须先调用本函数完成
    // frame/context drain；本函数不表示底层 present 生命周期已完成。
    void ProcessTasks();

    static Nullable<unique_ptr<GpuRuntime>> Create(const render::VulkanDeviceDescriptor& desc, render::VulkanInstanceDescriptor vkInsDesc);
    static Nullable<unique_ptr<GpuRuntime>> Create(const render::D3D12DeviceDescriptor& desc);

private:
    class SubmittedBatch {
    public:
        render::Fence* Fence{nullptr};
        uint64_t SignalValue{0};
    };

    class ResourceRetirement {
    public:
        GpuResourceHandle Handle{};
        vector<render::Fence*> Fences{};
        vector<uint64_t> Values{};
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

    render::Fence* GetQueueFence(render::QueueType type, uint32_t slot);
    SubmittedBatch SubmitContext(GpuAsyncContext* context, Nullable<render::SwapChainSyncObject*> waitToExecute, Nullable<render::SwapChainSyncObject*> readyToPresent);
    void CaptureAllQueueFenceSnapshots(ResourceRetirement& retirement);
    bool IsRetirementReady(const ResourceRetirement& retirement) const;
    void ProcessPendingResourceRetirements();
#ifdef RADRAY_IS_DEBUG
    void ValidateFrameContextForConsume(const char* apiName, const GpuFrameContext* context) const;
#endif
    SubmitFrameResult FinalizeFrame(unique_ptr<GpuFrameContext> context);
    GpuRuntime::BeginFrameResult AcquireSwapChain(GpuSurface* surface, uint64_t timeoutMs);

    shared_ptr<render::Device> _device;
    unique_ptr<render::InstanceVulkan> _vkInstance;
    std::array<vector<unique_ptr<render::Fence>>, (size_t)render::QueueType::MAX_COUNT> _queueFences;
    std::array<set<uint32_t>, (size_t)render::QueueType::MAX_COUNT> _knownQueueSlots;
    vector<Pending> _pendings;
    unique_ptr<GpuResourceRegistry> _resourceRegistry;
    vector<ResourceRetirement> _resourceRetirements;

    friend class GpuAsyncContext;
    friend class GpuFrameContext;
};

}  // namespace radray
