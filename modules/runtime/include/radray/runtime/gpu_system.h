#pragma once

#include <atomic>
#include <optional>
#include <limits>

#include <radray/types.h>
#include <radray/nullable.h>
#include <radray/render/common.h>

namespace radray {

class GpuRuntime;
class GpuSubmitContext;
class GpuPresentSurface;
class GpuTask;

// ---------------------------------------------------------------------------
// GpuSurfaceStatus
// surface 操作状态，用于 Acquire 返回值中。
// ---------------------------------------------------------------------------
enum class GpuSurfaceStatus : uint8_t {
    Success,     // 正常
    Suboptimal,  // 成功但尺寸不匹配，建议 Resize
    OutOfDate,   // 必须 Resize 后才能继续
    Lost,        // 不可恢复，需 Destroy 重建
};

struct GpuRuntimeDescriptor {
    render::RenderBackend Backend{render::RenderBackend::D3D12};
    bool EnableDebugValidation{false};
    render::RenderLogCallback LogCallback{nullptr};
    void* LogUserData{nullptr};
};

// ---------------------------------------------------------------------------
// GpuPresentSurfaceDescriptor
// 注意：这里只保留平台原生窗口句柄，不直接依赖 window 模块类型。
// 调用方必须保证句柄在 surface 生命周期内持续有效。
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

struct GpuResourceHandle {
    // Runtime 层分配的稳定资源标识。
    // - 由 GpuRuntime 在创建资源时生成
    // - 调用方不应自行构造或修改
    // - 生命周期语义由 runtime 决定；当资源失效后，该 Handle 也随之失效
    uint64_t Handle;

    // 后端 RHI 层对象的穿透指针。
    // - 该指针不转移所有权，只是一个 borrowed handle
    // - 具体指向什么对象由后端自行定义，例如某个 render 后端里的 Buffer/Texture 实现对象
    // - 其生命周期与 Handle 一致；当资源失效后，该指针也随之失效
    // - 调用方不应假设不同后端下它具有统一的具体类型
    void* NativeHandle;

    constexpr auto IsValid() const noexcept { return Handle != std::numeric_limits<uint64_t>::max(); }

    constexpr void Invalidate() noexcept {
        Handle = std::numeric_limits<uint64_t>::max();
        NativeHandle = nullptr;
    }

    constexpr static GpuResourceHandle Invalid() noexcept { return {std::numeric_limits<uint64_t>::max(), nullptr}; }
};

struct GpuSurfaceAcquireResult {
    GpuResourceHandle BackBuffer;  // 本帧 render target
    uint32_t FrameIndex;           // flight frame 槽位 [0, FlightFrameCount)
    GpuSurfaceStatus Status;       // 状态反馈
};

// ---------------------------------------------------------------------------
// GpuPresentSurface
// 管理 swapchain / present surface。对象由 GpuRuntime 创建并拥有底层 GPU 对象。
// surface 自身可长期存在；Acquire/Present 语义附着在某个 GpuSubmitContext 上。
// 也就是说，present 不是独立提交，而是“当前提交批次”的一部分。
// ---------------------------------------------------------------------------
class GpuPresentSurface {
public:
    ~GpuPresentSurface() noexcept;

    bool IsValid() const noexcept;
    void Destroy() noexcept;

    // 重建 swapchain。用于窗口大小变化或运行时切换 present mode / format。
    // 内部会同步等待所有 in-flight frame 完成，调用方无需手动 Wait。
    // 未指定的 format / presentMode 保持当前值不变。
    // 返回 false 表示重建失败（例如 surface 已 Lost）。
    bool Reconfigure(
        uint32_t width,
        uint32_t height,
        std::optional<render::TextureFormat> format = std::nullopt,
        std::optional<render::PresentMode> presentMode = std::nullopt) noexcept;

    uint32_t GetWidth() const noexcept;
    uint32_t GetHeight() const noexcept;
    render::TextureFormat GetFormat() const noexcept;
    render::PresentMode GetPresentMode() const noexcept;

private:
    GpuRuntime* _runtime{nullptr};
    const void* _nativeWindowHandle{nullptr};
    uint32_t _width{0};
    uint32_t _height{0};
    uint32_t _backBufferCount{0};
    uint32_t _flightFrameCount{0};
    render::TextureFormat _format{render::TextureFormat::BGRA8_UNORM};
    render::PresentMode _presentMode{render::PresentMode::FIFO};
    unique_ptr<render::SwapChain> _swapchain;
    render::CommandQueue* _presentQueue{nullptr};
    uint64_t _frameNumber{0};
    vector<render::TextureState> _backBufferStates;
    unique_ptr<render::Fence> _fence;
    uint64_t _submitCount{0};
    vector<uint64_t> _slotTargetValues;

    friend class GpuRuntime;
    friend class GpuSubmitContext;
};

// ---------------------------------------------------------------------------
// GpuTask
// 表示一次已经提交到 GPU 队列上的执行批次。
// 它不是 command buffer，也不是资源；它是一个通用的“任务 / 依赖令牌”概念。
//
// 语义约束：
// - 一个 GpuTask 对应一次 GpuRuntime::Submit(ctx)
// - 一个 GpuTask 只对应一个队列上的一次提交
// - 一个 GpuTask 表示“该提交批次执行完成”，不额外承诺 scanout / 显示完成
// - 提交期间所需的 command buffer / ctx 保活由 GpuRuntime 负责，不由 GpuTask 持有
// ---------------------------------------------------------------------------
class GpuTask {
public:
    GpuTask() noexcept = default;
    GpuTask(const GpuTask&) = delete;
    GpuTask& operator=(const GpuTask&) = delete;
    GpuTask(GpuTask&&) noexcept = default;
    GpuTask& operator=(GpuTask&&) noexcept = default;
    ~GpuTask() noexcept = default;

    bool IsValid() const noexcept;

    bool IsCompleted() const noexcept;

    void Wait() noexcept;

private:
    GpuRuntime* _runtime{nullptr};
    render::Fence* _fence{nullptr};
    uint64_t _targetValue{0};
    friend class GpuRuntime;
};

// ---------------------------------------------------------------------------
// GpuSubmitContext
// 表示一次 GPU 提交批次的 CPU 侧构建上下文。
//
// 语义约束：
// - 一个 GpuSubmitContext 只对应一次 Submit()
// - 一个 GpuSubmitContext 只绑定一个具体队列
// - 一个 GpuSubmitContext 可以聚合多条命令录制结果，统一提交到该队列
// - GpuSubmitContext 本身不是 command buffer，而是”提交批次”
// - Submit() 后该对象即被消费，不能复用
// - 未提交即析构是安全的，不会泄漏 GPU 资源（例如 TryAcquire 失败后直接丢弃 ctx）
// ---------------------------------------------------------------------------
class GpuSubmitContext {
public:
    ~GpuSubmitContext() noexcept = default;

    // 阻塞直到拿到可用 back buffer，或 surface 进入不可恢复状态。
    GpuSurfaceAcquireResult Acquire(GpuPresentSurface& surface) noexcept;

    // 向 ctx 注册"本次提交完成后需要 present 此 surface"的意图。
    // backBuffer 必须是本次 Acquire 返回的 BackBuffer handle。
    void Present(GpuPresentSurface& surface, GpuResourceHandle backBuffer) noexcept;

private:
    GpuRuntime* _runtime{nullptr};
    bool _queueBound{false};
    render::QueueType _queueType{render::QueueType::Direct};
    render::CommandQueue* _queue{nullptr};
    GpuPresentSurface* _acquiredSurface{nullptr};
    GpuResourceHandle _acquiredBackBuffer{GpuResourceHandle::Invalid()};
    bool _shouldPresent{false};
    uint32_t _frameSlotIndex{0};
    render::SwapChainSyncObject* _waitToDraw{nullptr};
    render::SwapChainSyncObject* _readyToPresent{nullptr};
    unique_ptr<render::CommandBuffer> _cmdBuffer;
    bool _consumed{false};

    friend class GpuRuntime;
};

// ---------------------------------------------------------------------------
// GpuRuntime
// 负责创建提交上下文、surface，以及把一次提交批次转换成一个 GpuTask。
// 对于已经 Submit 的 ctx，GpuRuntime 会在 GPU 完成前继续持有，避免提交资源过早释放。
// ---------------------------------------------------------------------------
class GpuRuntime {
public:
    ~GpuRuntime() noexcept;

    bool IsValid() const noexcept;

    void Destroy() noexcept;

    Nullable<unique_ptr<GpuPresentSurface>> CreatePresentSurface(const GpuPresentSurfaceDescriptor& desc) noexcept;

    // 开始构建一次新的提交批次。队列类型在 Acquire 时延迟绑定。
    Nullable<unique_ptr<GpuSubmitContext>> BeginSubmit() noexcept;

    // 提交并消费 ctx。
    // 返回的 GpuTask 表示”这整个提交批次”的执行状态。
    // 提交后的 ctx 及其关联 GPU 资源由 GpuRuntime 内部保活，直到该提交完成。
    GpuTask Submit(unique_ptr<GpuSubmitContext> ctx) noexcept;

    // 非阻塞处理已提交批次的生命周期管理。
    // 会回收 fence 已完成的 in-flight submit，并释放 runtime 持有的 ctx / cmd buffer 等资源。
    void ProcessSubmits() noexcept;

    static Nullable<unique_ptr<GpuRuntime>> Create(const GpuRuntimeDescriptor& desc) noexcept;

private:
    struct InFlightSubmit {
        render::Fence* Fence{nullptr};
        uint64_t TargetValue{0};
        unique_ptr<GpuSubmitContext> Context;
    };

    render::RenderBackend _backend{render::RenderBackend::D3D12};
    shared_ptr<render::Device> _device;
    unique_ptr<render::InstanceVulkan> _vkInstance;
    std::atomic<uint64_t> _nextHandleId{0};
    vector<InFlightSubmit> _inFlightSubmissions;

    friend class GpuPresentSurface;
    friend class GpuSubmitContext;
    friend class GpuTask;
};

}  // namespace radray
