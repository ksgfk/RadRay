#pragma once

#include <atomic>
#include <chrono>
#include <limits>
#include <mutex>
#include <optional>
#include <span>

#include <radray/types.h>
#include <radray/coroutine.h>
#include <radray/vertex_data.h>
#include <radray/render/common.h>
#include <radray/runtime/asset.h>
#include <radray/runtime/gpu_resource.h>
#include <radray/runtime/render_resource_recycler.h>
#include <radray/runtime/service_registry.h>
#include <radray/runtime/shader_variant_library.h>

namespace radray::render {
class CommandBuffer;
class Dxc;
}  // namespace radray::render

namespace radray {

class Application;
class AppWindow;
class WindowManager;
class AppFrameContext;
class StaticMesh;
class FrameUploadScheduler;
class BeginFrameUploadAwaitable;
class FrameUploadScope;
class WaitFrameUploadGpuAwaitable;
struct FrameUploadRecord;
class GpuSystem;

enum class FrameUploadStage {
    AwaitingFrame,
    InFrame,
    AwaitingFence,
    FenceComplete,
};

struct GpuSystemDescriptor {
    render::VulkanInstanceDescriptor VulkanInstance{};
    render::DXGIFactoryDescriptor DXGIFactory{};
    render::DeviceDescriptor Device{};
    uint32_t MainQueueIndex{0};
    uint32_t BackBufferCount{3};
    uint32_t FlightDataCount{2};
    bool EnableFrameProfiler{true};
};

/// 一条等待 GpuSystem 上传阶段 / GPU fence 的协程记录。由 FrameUploadScheduler 管理。
struct FrameUploadRecord : ManualCoroutineRecord {
    render::CommandBuffer* Cmd{nullptr};
    ResourceUploader* Uploader{nullptr};
    uint32_t FlightIndex{std::numeric_limits<uint32_t>::max()};
    FrameUploadStage CurrentStage{FrameUploadStage::AwaitingFrame};
};

/// AcquireWindow 成功返回的轻量视图。重量级的 SwapChainFrame / sync object
/// 留在 runtime 的 per-flight FlightSlot 里，应用只拿到 backbuffer + view。
/// 【不暴露同步对象】sync object 是提交细节，由 runtime 独占。
struct AppFrameTarget {
    AppWindow* Window{nullptr};
    render::Texture* BackBuffer{nullptr};
    render::TextureView* BackBufferView{nullptr};
    uint32_t BackBufferIndex{0};
};

struct GpuFenceSignal {
    render::Fence* Fence{nullptr};
    uint64_t Value{0};

    static constexpr GpuFenceSignal Invalid() noexcept { return GpuFenceSignal{}; }

    constexpr bool IsValid() const noexcept { return Fence != nullptr; }
};

struct GpuQueueFrameTrack {
    render::CommandQueue* Queue{nullptr};
    unique_ptr<render::Fence> Fence;
    std::atomic<uint64_t> NextFenceValue{1};
};

struct GpuFlightAcquiredTarget {
    AppWindow* Window{nullptr};
    render::SwapChainFrame Frame;
};

/// runtime 拥有的 per-flight 槽位。代表流水线一条槽位在不同阶段的完整状态，
/// 按所有权/阶段分三组，跨阶段的访问时序由 runner 的信号量 + retire 锁保证：
///  - 录制态：渲染线程（单线程模式即主线程）在 BeginFrameRecord→Render→
///    EndFrameRecordAndSubmit 期间独占；
///  - 计时态：游戏线程在帧开头写 FrameStartTime / 清 WaitForDestroy；
///  - 提交态:Signal 由 EndFrameRecordAndSubmit 写、retire/CompleteFlight 读后清。
struct GpuFlightSlot {
    using AcquiredTarget = GpuFlightAcquiredTarget;

    // —— 录制态（渲染线程独占）。CmdBuffer 池化复用，
    //    Targets 收集本帧 acquire 的全部窗口以支持多窗口/多 viewport。
    unique_ptr<render::CommandBuffer> CmdBuffer;
    unique_ptr<FrameResources> RenderResources;
    HostWriteBatch HostWrites;
    vector<AcquiredTarget> Targets;
    bool Submitted{false};
    bool Recording{false};

    // —— 计时态（游戏线程写）。
    std::chrono::steady_clock::time_point FrameStartTime{};
    vector<unique_ptr<render::RenderBase>> WaitForDestroy;

    // —— 提交态（渲染线程写，retire 经 _retireMutex 读后清）。
    GpuFenceSignal Signal;
};

struct AppFrameSubmitDescriptor {
    std::span<render::CommandBuffer*> CmdBuffers{};
    std::span<render::Fence*> SignalFences{};
    std::span<uint64_t> SignalValues{};
    std::span<render::Fence*> WaitFences{};
    std::span<uint64_t> WaitValues{};
};

class WaitFrameUploadGpuAwaitable {
public:
    WaitFrameUploadGpuAwaitable(FrameUploadScheduler* scheduler, FrameUploadRecord* record) noexcept
        : _scheduler(scheduler), _record(record) {}

    bool await_ready() noexcept;
    bool await_suspend(std::coroutine_handle<> h) noexcept;
    bool await_resume() noexcept;

private:
    FrameUploadScheduler* _scheduler;
    FrameUploadRecord* _record;
};

class FrameUploadScope {
public:
    FrameUploadScope() noexcept = default;

    render::CommandBuffer* GetCommandBuffer() const noexcept;
    ResourceUploader& GetUploader() const noexcept;
    uint32_t GetFlightIndex() const noexcept;

    task<void> WaitGpu();

private:
    friend class BeginFrameUploadAwaitable;
    FrameUploadScope(FrameUploadScheduler* scheduler, FrameUploadRecord* record) noexcept
        : _scheduler(scheduler), _record(record) {}
    FrameUploadScheduler* _scheduler{nullptr};
    FrameUploadRecord* _record{nullptr};
};

/// GpuSystem 专属的帧上传协程调度器。负责等待帧顶 upload phase 与 GPU fence。
class FrameUploadScheduler {
public:
    FrameUploadScheduler() noexcept = default;
    FrameUploadScheduler(const FrameUploadScheduler&) = delete;
    FrameUploadScheduler(FrameUploadScheduler&&) = delete;
    FrameUploadScheduler& operator=(const FrameUploadScheduler&) = delete;
    FrameUploadScheduler& operator=(FrameUploadScheduler&&) = delete;
    ~FrameUploadScheduler() noexcept;

    task<FrameUploadScope> BeginUpload();
    void RunUploadPhase(render::CommandBuffer* cmdBuffer, ResourceUploader& uploader, uint32_t flightIndex);
    void NotifyFlightComplete(uint32_t flightIndex);
    void PumpCompletedUploads();

    FrameUploadRecord* RegisterUpload(stop_token stop, std::coroutine_handle<> continuation);
    bool EraseUpload(FrameUploadRecord* record) noexcept;

private:
    bool IsUploadAlive(FrameUploadRecord* record) const noexcept;
    void ResumeRecord(FrameUploadRecord* record);
    void CancelRecord(FrameUploadRecord* record) noexcept;

    ManualCoroutineScheduler<FrameUploadRecord> _uploads;
};

/// co_await FrameUploadScheduler::BeginUpload() 的 awaitable。恢复点在 GpuSystem 帧顶 upload phase。
class BeginFrameUploadAwaitable {
public:
    BeginFrameUploadAwaitable(FrameUploadScheduler* scheduler, stop_token stop) noexcept
        : _scheduler(scheduler), _stop(stop) {}

    bool await_ready() const noexcept;
    bool await_suspend(std::coroutine_handle<> h);
    std::optional<FrameUploadScope> await_resume() noexcept;

private:
    FrameUploadScheduler* _scheduler;
    stop_token _stop;
    FrameUploadRecord* _record{nullptr};
};

/// 每帧 GPU 耗时探针。对应 UE5 的 FGPUTiming(最小化):per-flight timestamp pool + readback。
/// 由 GpuSystem 在 BeginFrameRecord/EndFrameRecordAndSubmit 自动包裹本帧录制,
/// CompleteFlight 时 resolve。应用只读 GetLastGpuTimeMs()。后端 readback barrier 差异内部隐藏。
class GpuFrameProfiler {
public:
    GpuFrameProfiler(render::Device* device, render::CommandQueue* queue, uint32_t flightCount);
    ~GpuFrameProfiler() noexcept;
    GpuFrameProfiler(const GpuFrameProfiler&) = delete;
    GpuFrameProfiler(GpuFrameProfiler&&) = delete;
    GpuFrameProfiler& operator=(const GpuFrameProfiler&) = delete;
    GpuFrameProfiler& operator=(GpuFrameProfiler&&) = delete;

    /// 帧顶(upload phase 之后):reset pool + 写 Top timestamp。
    void BeginFrame(render::CommandBuffer* cmdBuffer, uint32_t flightIndex);
    /// 帧尾(提交之前):写 Bottom timestamp + resolve 到 readback(含后端 barrier)。
    void EndFrame(render::CommandBuffer* cmdBuffer, uint32_t flightIndex);
    /// flight fence 完成后:读回并换算耗时。
    void Resolve(uint32_t flightIndex);

    float GetLastGpuTimeMs() const noexcept { return _lastGpuTimeMs; }

private:
    static constexpr uint32_t TimestampQueryCount = 2;

    struct FrameTiming {
        unique_ptr<render::QueryPool> Pool;
        unique_ptr<render::Buffer> Readback;
        bool Pending{false};
    };

    render::CommandQueue* _queue;
    bool _readbackNeedsBarrier{false};
    vector<FrameTiming> _frames;
    float _lastGpuTimeMs{0.0f};
};

struct DeferredRenderDeleteEntry {
    uint64_t TargetFenceValue{0};
    unique_ptr<render::RenderBase> Object;
};

class DeferredRenderDeleteQueue {
public:
    void Push(uint64_t targetFenceValue, unique_ptr<render::RenderBase> obj) noexcept;
    void Process(uint64_t completedFenceValue) noexcept;
    void Flush() noexcept;
    uint32_t Count() const noexcept;

private:
    mutable std::mutex _mutex;
    vector<DeferredRenderDeleteEntry> _entries;
};

/// Render 回调的唯一入参，封装一帧录制 API。
/// 生命周期仅限本次 Render 调用；runtime 在 BeginFrameRecord 构造并传入。
class AppFrameContext {
public:
    AppFrameContext(
        GpuSystem* gpuSystem,
        uint32_t flightIndex,
        std::chrono::duration<float> deltaTime,
        std::chrono::duration<float> lastFrameLatency,
        bool isInModalLoop) noexcept
        : _gpuSystem(gpuSystem),
          _flightIndex(flightIndex),
          _deltaTime(deltaTime),
          _lastFrameLatency(lastFrameLatency),
          _isInModalLoop(isInModalLoop) {}

    uint32_t FlightIndex() const noexcept { return _flightIndex; }
    std::chrono::duration<float> DeltaTime() const noexcept { return _deltaTime; }
    std::chrono::duration<float> LastFrameLatency() const noexcept { return _lastFrameLatency; }
    bool IsInModalLoop() const noexcept { return _isInModalLoop; }

    /// runtime 已 Begin() 的主 command buffer，应用所有录制（含 backbuffer barrier）的落点。
    render::CommandBuffer* GetCommandBuffer() const noexcept;

    /// 按需获取窗口呈现目标。内部 AcquireNextSwapChainFrame：
    /// RequireRecreate/RetryLater/Error/最小化 → nullopt（应用跳过该窗口）。
    /// 成功时把 SwapChainFrame 收进本帧 FlightSlot，返回 backbuffer + view。
    /// 【不录任何 barrier】backbuffer 初始翻转与 →Present 收尾全部由应用显式录。
    std::optional<AppFrameTarget> AcquireWindow(AppWindow* window);

    /// 绑定当前 flight 的上传器；EndFlight/CollectFlight 完全由 runtime 掌管。
    ResourceUploader& GetUploader() const noexcept;

    FrameResources& GetFrameResources() const noexcept;

    /// 逃生舱：直接拿底层设备处理建资源、自定义 compute 和 readback。
    render::Device* GetDevice() const noexcept;
    GpuSystem* GetGpuSystem() const noexcept { return _gpuSystem; }

    /// 提交并呈现当前帧。runtime 始终注入主 command buffer、flight batch、内部 fence
    /// 和 swapchain 同步；描述符中的对象仅作为附加提交内容。
    void SubmitFrame(const AppFrameSubmitDescriptor& desc = {});

private:
    GpuSystem* _gpuSystem;
    uint32_t _flightIndex;
    std::chrono::duration<float> _deltaTime;
    std::chrono::duration<float> _lastFrameLatency;
    bool _isInModalLoop;
};

class GpuSystem : public IRenderResourceRecycler {
public:
    using FenceSignal = GpuFenceSignal;
    using QueueFrameTrack = GpuQueueFrameTrack;
    using FlightSlot = GpuFlightSlot;

    GpuSystem(Application* app, const GpuSystemDescriptor& desc);
    GpuSystem(const GpuSystem&) = delete;
    GpuSystem(GpuSystem&&) = delete;
    GpuSystem& operator=(const GpuSystem&) = delete;
    GpuSystem& operator=(GpuSystem&&) = delete;
    ~GpuSystem() noexcept;

    void RecycleRenderResource(unique_ptr<render::RenderBase> obj) noexcept override;
    void ProcessDeferredDeletes() noexcept;
    void FlushAllDeferredDeletes() noexcept;

    bool CompleteFlight(uint32_t flightIndex);
    void WaitAndCleanupCompletedFlights();
    bool CompleteFlightIfReady(uint32_t flightIndex, bool wait);
    void BeginUpdateForFlight(uint32_t flightIndex);

    /// 一帧开头：取/建该 flight 的 CommandBuffer 并 Begin()，清空上帧 acquire 的目标。
    /// 返回供应用在 Render 中使用的帧上下文。
    AppFrameContext BeginFrameRecord(
        uint32_t flightIndex,
        std::chrono::duration<float> deltaTime,
        std::chrono::duration<float> lastFrameLatency,
        bool isInModalLoop);

    /// 一帧收尾：uploader.EndFlight → CmdBuffer.End → 聚合 sync object → Submit
    /// → 写 flight.Signal → Present 全部 target。
    void EndFrameRecordAndSubmit(uint32_t flightIndex);

    FrameUploadScheduler& GetFrameUploadScheduler() noexcept { return *_frameUploadScheduler; }
    void PumpFrameUploadScheduler();

    render::Device* GetDevice() const noexcept { return _device.get(); }
    PipelineLayoutLibrary* GetPipelineLayoutLibrary() const noexcept { return _pipelineLayoutLibrary.get(); }
    RenderPassRegistry* GetRenderPassRegistry() const noexcept { return _renderPassRegistry.get(); }
    render::CommandQueue* GetMainQueue() const noexcept { return _mainQueue; }
    WindowManager* GetWindowManager() const noexcept { return _windowManager; }
    /// 注入窗口系统(非拥有)。由装配阶段(ServiceRegistry / Application)调用。
    void SetWindowManager(WindowManager* windowManager) noexcept { _windowManager = windowManager; }
    uint32_t GetBackBufferCount() const noexcept { return _backBufferCount; }
    uint32_t GetFlightDataCount() const noexcept { return _flightDataCount; }
    uint64_t GetFrameIndex() const noexcept { return _nowFrameIndex; }
    uint32_t GetCurrentFlightIndex() const noexcept;
    FrameResources& GetFrameResources(uint32_t flightIndex) noexcept;
    std::chrono::duration<float> GetLastFrameLatency() const noexcept { return _lastFrameLatency; }
    void AdvanceFrameIndex() noexcept { ++_nowFrameIndex; }

    /// 上一帧 GPU 执行耗时(毫秒)。启用 GpuSystemDescriptor::EnableFrameProfiler 后由
    /// 内置 GpuFrameProfiler 在每帧 resolve 后更新；未启用时返回 0。
    float GetLastGpuTimeMs() const noexcept;

    UploadMemoryStats GetUploadMemoryStats() const noexcept;

private:
    friend class AppFrameContext;
    friend class Application;

    void SubmitFrame(uint32_t flightIndex, const AppFrameSubmitDescriptor& desc);

    Application* _app;
    WindowManager* _windowManager{nullptr};
    Nullable<render::InstanceVulkan*> _vulkanInstance{nullptr};
    unique_ptr<render::DXGIFactory> _dxgiFactory;
    shared_ptr<render::Device> _device;
    render::CommandQueue* _mainQueue{nullptr};
    const uint32_t _backBufferCount;
    const uint32_t _flightDataCount;
    QueueFrameTrack _mainQueueTrack;
    vector<FlightSlot> _flights;
    unique_ptr<ResourceUploader> _uploader;
    unique_ptr<FrameUploadScheduler> _frameUploadScheduler;
    unique_ptr<GpuFrameProfiler> _frameProfiler;
    unique_ptr<PipelineLayoutLibrary> _pipelineLayoutLibrary;
    unique_ptr<RenderPassRegistry> _renderPassRegistry;
    DeferredRenderDeleteQueue _deferredDeletes;
    uint64_t _nowFrameIndex{0};
    std::chrono::duration<float> _lastFrameLatency{};
};

template <>
struct ServiceTraits<GpuSystem> {
    static constexpr auto Inject = std::tuple{&GpuSystem::SetWindowManager};
};

template <>
struct RuntimeTypeTrait<GpuSystem> {
    static constexpr RuntimeTypeId value{0xe7c701b1, 0xcab6, 0x4be7, 0x94, 0xec, 0xfd, 0x8c, 0x6f, 0xd4, 0xf4, 0x68};
    using Bases = std::tuple<IRenderResourceRecycler>;
};

}  // namespace radray
