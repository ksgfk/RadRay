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
// - GpuRuntime::Submit 表示提交一个 batch。一个 batch 对应一次 runtime 定义的提交，
//   并以单个 fence/semaphore 完成令牌进行同步；返回的 GpuTask 表示该 batch 的完成状态。
// - 与 swapchain/surface 关联的帧命令由 runtime 封装；frame 的 acquire/present 生命周期
//   不直接作为底层 RHI primitive 暴露给外部模块。
// - GpuResourceHandle 故意保持无类型：它表示 runtime 稳定资源标识，并提供到底层对象的
//   interop 穿透能力，但不在该层统一资源具体类型。
// - 这里的“异步”仅指 CPU 与 GPU 执行异步；CPU 侧公开 API 的调用语义保持同步。
// - 错误处理以“状态 + 异常”混合方式向上传播：BeginFrame 的非致命 acquire 结果通过
//   BeginFrameResult::Status 返回；真正的 GPU 致命错误仍通过异常报告。
// - BeginFrame 当前只报告 RequireRecreate，不在 runtime 内自动重建 surface/swapchain。
// - TryBeginFrame 是非阻塞接口：当 frame slot 或 swapchain 当前不可用时返回 RetryLater。
// - CreateSurface / BeginAsync 的 slot 参数表示 queue 槽位(queue slot)。
// - DependsOn 用于表示当前异步上下文依赖于某个 GPU 任务完成，没有 CPU 同步语义
//

namespace radray {

class GpuRuntime;
class GpuTask;
class GpuSurface;
class GpuFrameContext;
class GpuAsyncContext;

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

private:
    GpuRuntime* _runtime{nullptr};
    render::Fence* _fence{nullptr};
    uint64_t _signalValue{0};

    friend class GpuAsyncContext;
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

    bool DependsOn(const GpuTask& task);
    render::CommandBuffer* CreateCommandBuffer();

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

    friend class GpuRuntime;
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

private:
    GpuSurface* _surface{nullptr};
    size_t _frameSlotIndex{std::numeric_limits<size_t>::max()};
    render::Texture* _backBuffer{nullptr};
    Nullable<render::SwapChainSyncObject*> _waitToDraw{nullptr};
    Nullable<render::SwapChainSyncObject*> _readyToPresent{nullptr};
    uint32_t _backBufferIndex{std::numeric_limits<uint32_t>::max()};

    friend class GpuRuntime;
};

class GpuRuntime {
public:
    struct BeginFrameResult {
        Nullable<unique_ptr<GpuFrameContext>> Context;
        render::SwapChainAcquireStatus Status{render::SwapChainAcquireStatus::Error};
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

    BeginFrameResult BeginFrame(GpuSurface* surface);
    BeginFrameResult TryBeginFrame(GpuSurface* surface);
    unique_ptr<GpuAsyncContext> BeginAsync(render::QueueType type, uint32_t queueSlot = 0);

    GpuTask Submit(unique_ptr<GpuAsyncContext> context);

    void ProcessTasks();

    static Nullable<unique_ptr<GpuRuntime>> Create(const render::VulkanDeviceDescriptor& desc, render::VulkanInstanceDescriptor vkInsDesc);
    static Nullable<unique_ptr<GpuRuntime>> Create(const render::D3D12DeviceDescriptor& desc);

private:
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
    GpuRuntime::BeginFrameResult AcquireSwapChain(GpuSurface* surface, uint64_t timeoutMs);

    shared_ptr<render::Device> _device;
    unique_ptr<render::InstanceVulkan> _vkInstance;
    std::array<vector<unique_ptr<render::Fence>>, (size_t)render::QueueType::MAX_COUNT> _queueFences;
    vector<Pending> _pendings;

    friend class GpuAsyncContext;
    friend class GpuFrameContext;
};

}  // namespace radray
