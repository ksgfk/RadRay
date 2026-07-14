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
#include <radray/runtime/render_pass_registry.h>
#include <radray/runtime/service_registry.h>
#include <radray/runtime/shader_variant_library.h>

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

struct UploadMemoryStats {
    uint64_t PageCount{0};
    uint64_t PageCapacityBytes{0};
    uint64_t CommitCount{0};
    uint64_t CommittedBytes{0};
    uint64_t RecordedRangeCount{0};
    uint64_t FlushedRangeCount{0};
};

class HostWriteBatch {
public:
    HostWriteBatch();

    void Record(render::Buffer* target, render::BufferRange range);
    void Flush(render::Device& device) noexcept;

    bool Empty() const noexcept { return _ranges.empty(); }
    bool IsSealed() const noexcept { return _sealed; }
    std::span<const render::MappedBufferRange> GetRanges() const noexcept { return _ranges; }
    const UploadMemoryStats& GetStats() const noexcept { return _stats; }

    void Seal() noexcept { _sealed = true; }
    void Reset() noexcept;
    void RecordPageAllocation(uint64_t capacity) noexcept;

private:
    vector<render::MappedBufferRange> _ranges;
    UploadMemoryStats _stats{};
    bool _sealed{false};
};

class ScopedBufferMap {
public:
    ScopedBufferMap(render::Buffer* buffer, render::BufferRange range) noexcept;
    ~ScopedBufferMap() noexcept;

    ScopedBufferMap(const ScopedBufferMap&) = delete;
    ScopedBufferMap& operator=(const ScopedBufferMap&) = delete;
    ScopedBufferMap(ScopedBufferMap&&) = delete;
    ScopedBufferMap& operator=(ScopedBufferMap&&) = delete;

    void* Data() const noexcept { return _data; }

    template <typename T>
    T* DataAs() const noexcept {
        return static_cast<T*>(_data);
    }

    explicit operator bool() const noexcept { return _data != nullptr; }

private:
    render::Buffer* _buffer{nullptr};
    void* _data{nullptr};
    render::BufferRange _range{};
    bool _write{false};
};

enum class FrameUploadStage {
    AwaitingFrame,
    InFrame,
    AwaitingFence,
    FenceComplete,
};

struct GpuSystemDescriptor {
    render::Device* Device;
    uint32_t MainQueueIndex;
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

    render::Device* GetDevice() const noexcept { return _device; }
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
    render::Device* _device;
    render::CommandQueue* _mainQueue;
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

/// Upload heap staging page 池。在 page 内线性分配，按 flight index 整页回收。
class StagingBufferPool {
public:
    using Allocation = MappedUploadPage::Allocation;
    using Reservation = MappedUploadPage::Reservation;

    struct Descriptor {
        uint64_t PageSize{8ull * 1024 * 1024};
        uint64_t MaxCachedBytes{64ull * 1024 * 1024};
        uint32_t MaxCachedPages{8};
    };

    StagingBufferPool(render::Device* device, uint32_t flightCount, const Descriptor& desc) noexcept;
    explicit StagingBufferPool(render::Device* device, uint32_t flightCount) noexcept;
    ~StagingBufferPool() noexcept;
    StagingBufferPool(const StagingBufferPool&) = delete;
    StagingBufferPool& operator=(const StagingBufferPool&) = delete;

    void BeginFlight(HostWriteBatch& hostWrites);

    /// 从 Upload page 预留一块 staging 内存。超过 page 容量的请求使用一次性 buffer。
    Reservation Reserve(uint64_t size, uint64_t alignment = 1);

    /// 将当前所有活跃 staging page 移入指定 flight 的 pending 列表。
    void RetireToFlight(uint32_t flightIndex);

    /// 回收指定 flight 的标准 page 到 free list，释放一次性 buffer。
    void CollectFlight(uint32_t flightIndex);

private:
    struct Page {
        unique_ptr<MappedUploadPage> Upload;
        bool Cacheable{true};
    };

    Page CreatePage(uint64_t capacity, bool cacheable);
    Page& AcquireStandardPage();
    void TrimFreeList() noexcept;

    render::Device* _device;
    Descriptor _desc;
    vector<Page> _active;
    vector<vector<Page>> _pending;
    vector<Page> _freeList;
    uint64_t _nextPageId{0};
    HostWriteBatch* _hostWrites{nullptr};
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

    void BeginFlight(uint32_t flightIndex, HostWriteBatch& hostWrites);

    /// 录制 buffer 上传命令到 cmdBuffer。
    void UploadBuffer(
        render::CommandBuffer* cmdBuffer,
        const BufferUploadRequest& request);

    /// 录制 texture 上传命令到 cmdBuffer。
    void UploadTexture(
        render::CommandBuffer* cmdBuffer,
        const TextureUploadRequest& request);

    /// 从 CPU 网格数据创建 device-local buffer 并录制上传命令，产出 GpuMesh。
    /// 产出的 buffer 所有权在返回的 GpuMesh 中，由调用方(加载协程/资产)持有。
    std::optional<GpuMesh> UploadMeshResource(
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
    uint32_t _flightCount{0};
    uint32_t _activeFlightIndex{std::numeric_limits<uint32_t>::max()};
};

/// 依赖声明(非侵入,类外特化):GpuSystem 需要 WindowManager,复用已有 public setter。
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
