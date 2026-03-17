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
// Internal completion point
// ---------------------------------------------------------------------------
struct GpuCompletionPoint {
    shared_ptr<render::Fence> Fence;
    uint64_t TargetValue{0};
};

class GpuCompletionState {
public:
    vector<GpuCompletionPoint> Points;
};

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
    Success,
    Unavailable,
    Suboptimal,
    OutOfDate,
    Lost,
};

// ---------------------------------------------------------------------------
// GpuSurfaceLease
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
// ---------------------------------------------------------------------------
struct GpuQueueSubmitStep {
    render::QueueType Queue{render::QueueType::Direct};

    vector<uint32_t> WaitStepIndices;
    vector<unique_ptr<render::CommandBuffer>> CommandBuffers;
};

// ---------------------------------------------------------------------------
// GpuPresentSurface
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

    uint64_t _frameNumber{0};
    vector<shared_ptr<GpuCompletionState>> _slotRetireStates;

    friend class GpuRuntime;
    friend class GpuFrameContext;
    friend class GpuSurfaceLease;
};

// ---------------------------------------------------------------------------
// GpuFrameContext
// ---------------------------------------------------------------------------
class GpuFrameContext {
public:
    ~GpuFrameContext() noexcept = default;

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

private:
    GpuRuntime* _runtime{nullptr};
    vector<unique_ptr<GpuSurfaceLease>> _surfaceLeases;
    vector<shared_ptr<GpuCompletionState>> _dependencies;

    vector<unique_ptr<render::CommandBuffer>> _defaultGraphicsCmds;
    vector<unique_ptr<render::CommandBuffer>> _defaultComputeCmds;
    vector<unique_ptr<render::CommandBuffer>> _defaultCopyCmds;

    vector<GpuQueueSubmitStep> _submissionPlan;
    bool _consumed{false};

    friend class GpuRuntime;
};

// ---------------------------------------------------------------------------
// GpuAsyncContext
// ---------------------------------------------------------------------------
class GpuAsyncContext {
public:
    ~GpuAsyncContext() noexcept = default;

    bool WaitFor(const GpuTask& task) noexcept;

    bool AddGraphicsCommandBuffer(unique_ptr<render::CommandBuffer> cmd) noexcept;
    bool AddComputeCommandBuffer(unique_ptr<render::CommandBuffer> cmd) noexcept;
    bool AddCopyCommandBuffer(unique_ptr<render::CommandBuffer> cmd) noexcept;

    bool SetSubmissionPlan(vector<GpuQueueSubmitStep> steps) noexcept;
    bool IsEmpty() const noexcept;

    Nullable<unique_ptr<render::CommandBuffer>> CreateGraphicsCommandBuffer() noexcept;
    Nullable<unique_ptr<render::CommandBuffer>> CreateComputeCommandBuffer() noexcept;
    Nullable<unique_ptr<render::CommandBuffer>> CreateCopyCommandBuffer() noexcept;

private:
    GpuRuntime* _runtime{nullptr};
    vector<shared_ptr<GpuCompletionState>> _dependencies;

    vector<unique_ptr<render::CommandBuffer>> _defaultGraphicsCmds;
    vector<unique_ptr<render::CommandBuffer>> _defaultComputeCmds;
    vector<unique_ptr<render::CommandBuffer>> _defaultCopyCmds;

    vector<GpuQueueSubmitStep> _submissionPlan;
    bool _consumed{false};

    friend class GpuRuntime;
};

// ---------------------------------------------------------------------------
// GpuRuntime
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
