#pragma once

#include <array>
#include <mutex>
#include <optional>
#include <limits>

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
// - 错误处理目前统一通过异常向上传播；所有 GPU 相关错误都视为致命且不可恢复，
//   调用方应在最外层记录错误并终止程序。
// - TryBeginFrame 是非阻塞探测接口：当当前没有可获取帧时返回空；致命错误仍通过异常报告。
// - CreateSurface / BeginAsync 的 slot 参数表示 queue 槽位(queue slot)。
//

namespace radray {

class GpuRuntime;
class GpuTask;
class GpuSurface;
class GpuFrameContext;
class GpuAsyncContext;

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
    GpuTask(GpuRuntime* runtime, GpuResourceHandle fence, uint64_t signalValue) noexcept;
    GpuTask(const GpuTask&) noexcept = delete;
    GpuTask& operator=(const GpuTask&) noexcept = delete;
    GpuTask(GpuTask&&) noexcept = default;
    GpuTask& operator=(GpuTask&&) noexcept = default;
    ~GpuTask() noexcept = default;

    bool IsValid() const;
    bool IsCompleted() const;
    void Wait();

private:
    friend class GpuAsyncContext;
    friend class GpuRuntime;

    GpuRuntime* _runtime{nullptr};
    GpuResourceHandle _fence;
    uint64_t _signalValue{0};
};

class GpuSurface {
public:
    ~GpuSurface() noexcept;

    bool IsValid() const;
    void Destroy();

    uint32_t GetWidth() const;
    uint32_t GetHeight() const;
    render::TextureFormat GetFormat() const;
    render::PresentMode GetPresentMode() const;

private:
    friend class GpuRuntime;

    struct PendingFrame {
        render::Fence* Fence{nullptr};
        uint64_t SignalValue{0};
    };

    GpuSurface(
        GpuRuntime* runtime,
        unique_ptr<render::SwapChain> swapchain,
        const render::SwapChainDescriptor& desc,
        uint32_t queueSlot) noexcept;

    GpuRuntime* _runtime{nullptr};
    unique_ptr<render::SwapChain> _swapchain;
    render::SwapChainDescriptor _desc{};
    uint32_t _queueSlot{0};
    vector<GpuResourceHandle> _backBuffers;
    vector<render::TextureState> _backBufferStates;
    deque<PendingFrame> _inFlightFrames;
    mutable std::mutex _mutex;
};

class GpuAsyncContext {
public:
    virtual ~GpuAsyncContext() noexcept;

    bool IsEmpty() const;
    /** GPU 侧等待语义, CPU 不会做 wait */
    bool DependsOn(const GpuTask& task);

private:
    friend class GpuRuntime;
    friend class GpuFrameContext;

    GpuAsyncContext(GpuRuntime* runtime, render::QueueType type, uint32_t queueSlot) noexcept;

    GpuRuntime* _runtime{nullptr};
    render::QueueType _queueType{render::QueueType::Direct};
    uint32_t _queueSlot{0};
    vector<render::Fence*> _waitFences;
    vector<uint64_t> _waitValues;
    bool _isFrameContext{false};
};

class GpuFrameContext : public GpuAsyncContext {
public:
    ~GpuFrameContext() noexcept override;

    GpuResourceHandle GetBackBuffer() const;
    uint32_t GetBackBufferIndex() const;

private:
    friend class GpuRuntime;

    GpuFrameContext(
        GpuRuntime* runtime,
        GpuSurface* surface,
        uint32_t queueSlot,
        GpuResourceHandle backBuffer,
        uint32_t backBufferIndex,
        render::SwapChainSyncObject* waitToDraw,
        render::SwapChainSyncObject* readyToPresent,
        unique_ptr<render::CommandBuffer> internalCmdBuffer) noexcept;

    GpuSurface* _surface{nullptr};
    GpuResourceHandle _backBuffer;
    uint32_t _backBufferIndex{std::numeric_limits<uint32_t>::max()};
    render::SwapChainSyncObject* _waitToDraw{nullptr};
    render::SwapChainSyncObject* _readyToPresent{nullptr};
    unique_ptr<render::CommandBuffer> _internalCmdBuffer;
};

class GpuRuntime {
public:
    GpuRuntime(const GpuRuntime&) noexcept = delete;
    GpuRuntime& operator=(const GpuRuntime&) noexcept = delete;
    GpuRuntime(GpuRuntime&&) noexcept = delete;
    GpuRuntime& operator=(GpuRuntime&&) noexcept = delete;
    ~GpuRuntime() noexcept;

    bool IsValid() const;
    void Destroy();

    unique_ptr<GpuSurface> CreateSurface(const render::SwapChainDescriptor& desc, uint32_t queueSlot = 0);

    unique_ptr<GpuFrameContext> BeginFrame(GpuSurface* surface);
    Nullable<unique_ptr<GpuFrameContext>> TryBeginFrame(GpuSurface* surface);
    unique_ptr<GpuAsyncContext> BeginAsync(render::QueueType type, uint32_t queueSlot = 0);

    GpuTask Submit(unique_ptr<GpuAsyncContext> context);

    void ProcessTasks();

    static Nullable<unique_ptr<GpuRuntime>> Create(const render::DeviceDescriptor& desc, std::optional<render::VulkanInstanceDescriptor> vkInsDesc);

private:
    friend class GpuTask;
    friend class GpuSurface;

    struct QueueState {
        struct PendingSubmission {
            uint64_t SignalValue{0};
            unique_ptr<render::CommandBuffer> InternalCommandBuffer;
        };

        render::CommandQueue* Queue{nullptr};
        unique_ptr<render::Fence> Fence;
        GpuResourceHandle FenceHandle;
        uint64_t NextSignalValue{1};
        uint64_t LastWaitedSignalValue{0};
        deque<PendingSubmission> PendingSubmissions;
        std::mutex Mutex;
    };

    struct ResourceRegistryEntry {
        void* NativeHandle{nullptr};
    };

    GpuRuntime(shared_ptr<render::Device> device, unique_ptr<render::InstanceVulkan> vkInstance) noexcept;

    QueueState& EnsureQueueState(render::QueueType type, uint32_t queueSlot);
    GpuResourceHandle RegisterHandle(void* nativeHandle);
    void UnregisterHandle(GpuResourceHandle handle) noexcept;
    void OnTaskWaited(render::Fence* fence, uint64_t signalValue) noexcept;
    void RetireCompletedFrames(GpuSurface* surface) noexcept;
    GpuResourceHandle EnsureBackBufferHandle(GpuSurface* surface, uint32_t backBufferIndex, render::Texture* backBuffer);
    unique_ptr<GpuFrameContext> CreateFrameContext(GpuSurface* surface, const render::AcquireResult& acquired);

    shared_ptr<render::Device> _device;
    unique_ptr<render::InstanceVulkan> _vkInstance;
    std::array<vector<unique_ptr<QueueState>>, static_cast<size_t>(render::QueueType::MAX_COUNT)> _queues;
    unordered_map<uint64_t, ResourceRegistryEntry> _resourceRegistry;
    unordered_set<GpuSurface*> _surfaces;
    uint64_t _nextResourceHandle{1};
    mutable std::mutex _mutex;
};

}  // namespace radray
