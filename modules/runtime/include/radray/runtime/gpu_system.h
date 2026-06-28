#pragma once

#include <atomic>
#include <chrono>
#include <coroutine>
#include <limits>
#include <mutex>
#include <optional>
#include <span>

#include <radray/types.h>
#include <radray/coroutine.h>
#include <radray/render/common.h>
#include <radray/render/gpu_resource.h>
#include <radray/runtime/asset.h>
#include <radray/runtime/render_resource_recycler.h>
#include <radray/runtime/service_registry.h>

namespace radray::render {
class CommandBuffer;
class Dxc;
}

namespace radray {

class Application;
class AppWindow;
class WindowManager;
class AppFrameContext;
class ResourceUploader;
class StaticMesh;
class FrameUploadScheduler;
class BeginFrameUploadAwaitable;
class FrameUploadScope;
class WaitFrameUploadGpuAwaitable;
struct FrameUploadRecord;

enum class FrameUploadStage {
    AwaitingFrame,
    InFrame,
    AwaitingFence,
    FenceComplete,
};

struct FrameUploadStopCallback {
    FrameUploadScheduler* Scheduler{nullptr};
    FrameUploadRecord* Record{nullptr};

    void operator()() const noexcept;
};

struct GpuSystemDescriptor {
    render::Device* Device;
    uint32_t MainQueueIndex;
    uint32_t BackBufferCount{3};
    uint32_t FlightDataCount{2};
    bool EnableFrameProfiler{true};
};

/// 一条等待 GpuSystem 上传阶段 / GPU fence 的协程记录。由 FrameUploadScheduler 管理。
struct FrameUploadRecord {
    using StopCallbackStorage = stop_token::template callback_type<FrameUploadStopCallback>;

    FrameUploadScheduler* Scheduler{nullptr};
    std::coroutine_handle<> Continuation{};
    stop_token Stop;
    std::optional<StopCallbackStorage> StopCallback;
    render::CommandBuffer* Cmd{nullptr};
    ResourceUploader* Uploader{nullptr};
    uint32_t FlightIndex{std::numeric_limits<uint32_t>::max()};
    FrameUploadStage CurrentStage{FrameUploadStage::AwaitingFrame};
    bool Canceled{false};
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
    vector<AcquiredTarget> Targets;
    bool ManualSubmit{false};
    bool Recording{false};

    // —— 计时态（游戏线程写）。
    std::chrono::steady_clock::time_point FrameStartTime{};
    vector<unique_ptr<render::RenderBase>> WaitForDestroy;

    // —— 提交态（渲染线程写，retire 经 _retireMutex 读后清）。
    GpuFenceSignal Signal;
};

/// 描述一次 buffer 上传操作。
struct BufferUploadRequest {
    std::span<const byte> SrcData;
    render::Buffer* DstBuffer;
    uint64_t DstOffset{0};
    render::BufferStates Before{render::BufferState::Common};
    render::BufferStates After{render::BufferState::Common};
};

/// 描述一次 texture 上传操作。
struct TextureUploadRequest {
    std::span<const byte> SrcData;
    render::Texture* DstTexture;
    render::SubresourceRange DstRange;
    uint64_t SrcRowPitch{0};
    render::TextureStates Before{render::TextureState::Undefined};
    render::TextureStates After{render::TextureState::ShaderRead};
};

struct StagingBufferAllocation {
    render::Buffer* Buffer{nullptr};
    void* MappedPtr{nullptr};
    uint64_t Offset{0};
    uint64_t Size{0};
};

class WaitFrameUploadGpuAwaitable {
public:
    explicit WaitFrameUploadGpuAwaitable(FrameUploadRecord* record) noexcept : _record(record) {}

    bool await_ready() noexcept;
    bool await_suspend(std::coroutine_handle<> h) noexcept;
    bool await_resume() noexcept;

private:
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
    explicit FrameUploadScope(FrameUploadRecord* record) noexcept : _record(record) {}
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
    friend struct FrameUploadStopCallback;

    bool IsUploadAlive(FrameUploadRecord* record) const noexcept;
    void ResumeRecord(FrameUploadRecord* record);
    void CancelRecord(FrameUploadRecord* record) noexcept;

    vector<unique_ptr<FrameUploadRecord>> _uploads;
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
    /// → 写 flight.Signal → Present 全部 target。ManualSubmit 下仅 End，跳过提交/呈现。
    void EndFrameRecordAndSubmit(uint32_t flightIndex);

    FrameUploadScheduler& GetFrameUploadScheduler() noexcept { return *_frameUploadScheduler; }
    void PumpFrameUploadScheduler();

    render::Device* GetDevice() const noexcept { return _device; }
    render::CommandQueue* GetMainQueue() const noexcept { return _mainQueue; }
    WindowManager* GetWindowManager() const noexcept { return _windowManager; }
    /// 注入窗口系统(非拥有)。由装配阶段(ServiceRegistry / Application)调用。
    void SetWindowManager(WindowManager* windowManager) noexcept { _windowManager = windowManager; }
    uint32_t GetBackBufferCount() const noexcept { return _backBufferCount; }
    uint32_t GetFlightDataCount() const noexcept { return _flightDataCount; }
    uint64_t GetFrameIndex() const noexcept { return _nowFrameIndex; }
    uint32_t GetCurrentFlightIndex() const noexcept;
    std::chrono::duration<float> GetLastFrameLatency() const noexcept { return _lastFrameLatency; }
    void AdvanceFrameIndex() noexcept { ++_nowFrameIndex; }

    /// 上一帧 GPU 执行耗时(毫秒)。启用 GpuSystemDescriptor::EnableFrameProfiler 后由
    /// 内置 GpuFrameProfiler 在每帧 resolve 后更新；未启用时返回 0。
    float GetLastGpuTimeMs() const noexcept;

private:
    friend class AppFrameContext;
    friend class Application;

    Application* _app;
    WindowManager* _windowManager{nullptr};
    render::Device* _device;
    render::CommandQueue* _mainQueue;
    const uint32_t _backBufferCount;
    const uint32_t _flightDataCount;
    QueueFrameTrack _mainQueueTrack;
    vector<FlightSlot> _flights;
    unique_ptr<ResourceUploader> _uploader;
    unique_ptr<FrameUploadScheduler> _frameUploadScheduler;
    unique_ptr<GpuFrameProfiler> _frameProfiler;
    DeferredRenderDeleteQueue _deferredDeletes;
    uint64_t _nowFrameIndex{0};
    std::chrono::duration<float> _lastFrameLatency{};
};

template <>
struct RuntimeTypeTrait<GpuSystem> {
    static constexpr RuntimeTypeId value{0xe7c701b1, 0xcab6, 0x4be7, 0x94, 0xec, 0xfd, 0x8c, 0x6f, 0xd4, 0xf4, 0x68};
};

/// 依赖声明(非侵入,类外特化):GpuSystem 需要 WindowManager,复用已有 public setter。
template <>
struct ServiceTraits<GpuSystem> {
    static constexpr auto Inject = std::tuple{&GpuSystem::SetWindowManager};
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

    /// 逃生舱：直接拿底层对象自行处理（建资源、自定义 compute、readback、甚至自行 submit）。
    render::Device* GetDevice() const noexcept;
    render::CommandQueue* GetMainQueue() const noexcept;
    GpuSystem* GetGpuSystem() const noexcept { return _gpuSystem; }

    /// 声明本帧应用自管提交，runtime 跳过 Submit/Present（仍 End cmd buffer 与回收 flight）。
    void SetManualSubmit() noexcept;

private:
    GpuSystem* _gpuSystem;
    uint32_t _flightIndex;
    std::chrono::duration<float> _deltaTime;
    std::chrono::duration<float> _lastFrameLatency;
    bool _isInModalLoop;
};

/// Upload heap staging buffer 池。按 flight index 管理回收。
class StagingBufferPool {
public:
    using Allocation = StagingBufferAllocation;

    explicit StagingBufferPool(render::Device* device, uint32_t flightCount) noexcept;
    ~StagingBufferPool() noexcept;
    StagingBufferPool(const StagingBufferPool&) = delete;
    StagingBufferPool& operator=(const StagingBufferPool&) = delete;

    /// 从 Upload heap 分配一块 staging 内存。
    Allocation Allocate(uint64_t size);

    /// 刷新并解除 staging 内存映射。
    void FlushAndUnmap(const Allocation& allocation);

    /// 将当前所有活跃 staging buffer 移入指定 flight 的 pending 列表。
    void RetireToFlight(uint32_t flightIndex);

    /// 回收指定 flight 的 staging buffer 到 free list。
    void CollectFlight(uint32_t flightIndex);

private:
    struct ActiveBuffer {
        unique_ptr<render::Buffer> Buffer;
        bool IsMapped{false};
        uint64_t MappedSize{0};
    };

    render::Device* _device;
    vector<ActiveBuffer> _active;
    vector<vector<unique_ptr<render::Buffer>>> _pending;
    vector<unique_ptr<render::Buffer>> _freeList;
};

/// 资源上传器。录制 copy 命令到外部传入的 CommandBuffer，管理 staging 生命周期。
///
/// 使用流程：
/// 1. 调用方 cmdBuffer->Begin()
/// 2. 调用 UploadBuffer / UploadTexture / UploadMeshResource（可多次）
/// 3. 调用 EndFlight(flightIndex) 将 staging 绑定到该 flight
/// 4. 调用方 cmdBuffer->End() → Submit → signal fence
/// 5. CompleteFlight 后调用 CollectFlight(flightIndex) 回收
class ResourceUploader {
public:
    ResourceUploader(render::Device* device, uint32_t flightCount);
    ~ResourceUploader() noexcept;
    ResourceUploader(const ResourceUploader&) = delete;
    ResourceUploader& operator=(const ResourceUploader&) = delete;

    /// 录制 buffer 上传命令到 cmdBuffer。
    void UploadBuffer(
        render::CommandBuffer* cmdBuffer,
        const BufferUploadRequest& request);

    /// 录制 texture 上传命令到 cmdBuffer。
    void UploadTexture(
        render::CommandBuffer* cmdBuffer,
        const TextureUploadRequest& request);

    /// 从 CPU 网格数据创建 device-local buffer 并录制上传命令，产出 RenderMesh。
    /// 产出的 buffer 所有权在返回的 RenderMesh 中，由调用方(加载协程/资产)持有。
    std::optional<render::RenderMesh> UploadMeshResource(
        render::CommandBuffer* cmdBuffer,
        const MeshResource& meshResource);

    /// 帧末：将本帧 staging 移入指定 flight 等待 GPU 完成。
    void EndFlight(uint32_t flightIndex);

    /// CompleteFlight 后：回收 staging buffer。
    void CollectFlight(uint32_t flightIndex);

    /// 底层设备(供加载协程在 upload phase 内建 device 资源,如纹理)。
    render::Device* GetDevice() const noexcept { return _device; }

private:
    render::Device* _device;
    StagingBufferPool _stagingPool;
};

}  // namespace radray
