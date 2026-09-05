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
#include <radray/render/rhi.h>
#include <radray/runtime/asset.h>
#include <radray/runtime/gpu_resource.h>
#include <radray/runtime/wait_frame.h>
#include <radray/runtime/service_traits.h>

// device / queue / flight / 上传 / 帧边界等待。帧序与关停顺序: docs/architecture/frame-and-gpu.md

namespace radray::render {
class CommandBuffer;
}  // namespace radray::render

namespace radray {

class Application;
class AppWindow;
class WindowManager;
class AppFrameContext;
class FrameUploadScheduler;
class BeginFrameUploadAwaitable;
class FrameUploadScope;
class WaitFrameUploadGpuAwaitable;
class WaitFrameAwaitable;
struct FrameUploadRecord;
struct WaitFrameRecord;
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

/// 一条等待帧边界的协程记录(IWaitFrameProcessor::Wait 的挂起点)。挂在某个 flight 上,
/// 该 flight 的 fence 完成后被标记 ready,再由主线程的 PumpWaitFrame 恢复。
struct WaitFrameRecord : ManualCoroutineRecord {
    /// 记录所属的 flight。记录存在期内不变 —— 摘除时要靠它定位所在的表。
    uint32_t FlightIndex{std::numeric_limits<uint32_t>::max()};
    bool FlightComplete{false};
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
///  - 计时态：游戏线程在帧开头写 FrameStartTime；
///  - 提交态:Signal 由 EndFrameRecordAndSubmit 写、retire/CompleteFlight 读后清。
struct GpuFlightSlot {
    using AcquiredTarget = GpuFlightAcquiredTarget;

    // —— 录制态（渲染线程独占）。CmdBuffer 池化复用，
    //    Targets 收集本帧 acquire 的全部窗口以支持多窗口/多 viewport。
    unique_ptr<render::CommandBuffer> CmdBuffer;
    HostWriteBatch HostWrites;
    vector<AcquiredTarget> Targets;
    bool Submitted{false};
    bool Recording{false};

    // —— 计时态（游戏线程写）。
    std::chrono::steady_clock::time_point FrameStartTime{};

    /// 等在本 flight 帧边界上的协程 (IWaitFrameProcessor::Wait 的挂起点)。
    /// 【挂在这里而不是全局表】flight 的 fence 就是完成条件, 无需另存 fence 值再比较。
    /// 【两段式】CompleteFlight 只标记 FlightComplete (可能在渲染线程), resume 由主线程的
    /// PumpWaitFrame 做。
    ManualCoroutineScheduler<WaitFrameRecord> WaitFrame;

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
    /// Run and Pump are serialized by the runner; consume completions before reusing a flight.
    void RunUploadPhase(render::CommandBuffer* cmdBuffer, ResourceUploader& uploader, uint32_t flightIndex);
    /// May run concurrently with Run/Pump; only queues a completed flight notification.
    void NotifyFlightComplete(uint32_t flightIndex);
    /// Resume completed/canceled loads on the game thread.
    void PumpCompletedUploads();

    FrameUploadRecord* RegisterUpload(stop_token stop, std::coroutine_handle<> continuation);
    bool EraseUpload(FrameUploadRecord* record) noexcept;

private:
    bool IsUploadAlive(FrameUploadRecord* record) const noexcept;
    void ResumeRecord(FrameUploadRecord* record);
    void CancelRecord(FrameUploadRecord* record) noexcept;
    void ApplyCompletedFlights();

    ManualCoroutineScheduler<FrameUploadRecord> _uploads;
    std::mutex _completedFlightsMutex;
    vector<uint32_t> _completedFlights;
};

/// co_await GpuSystem::Wait() 的 awaitable。恢复点在 GpuSystem::PumpWaitFrame(主线程)。
class WaitFrameAwaitable {
public:
    WaitFrameAwaitable(GpuSystem* gpuSystem, stop_token stop) noexcept
        : _gpuSystem(gpuSystem), _stop(stop) {}

    bool await_ready() const noexcept;
    bool await_suspend(std::coroutine_handle<> h);
    void await_resume() noexcept;

private:
    GpuSystem* _gpuSystem;
    stop_token _stop;
    WaitFrameRecord* _record{nullptr};
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

    float GetLastGpuTimeMs() const noexcept { return _lastGpuTimeMs.load(std::memory_order_relaxed); }

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
    std::atomic<float> _lastGpuTimeMs{0.0f};
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

    HostWriteBatch& GetHostWrites() const noexcept;

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

/// runtime 侧的 GPU 设备与帧节奏所有者。职责边界:
/// - 拥有 instance / factory / device / 主队列 / fence,以及 flight 槽位、上传器、
///   帧 profiler 和帧边界等待表。
/// - 负责"何时画":BeginFrameRecord / EndFrameRecordAndSubmit 的录制与提交时序、
///   flight 回收与 GPU 资源生命周期兜底。
/// - 不关心"画什么":RenderPass / Framebuffer 缓存、pipeline、Scene 归 RenderSystem。
class GpuSystem : public IWaitFrameProcessor {
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

    /// IWaitFrameProcessor。挂进【当前】flight 的等待表 —— 调用点在帧顶 Update 期间,
    /// 此刻"已录制的 work"全属于更早的 flight, 故等当前 flight 的 fence 必然够。
    /// 代价是最多多等一轮, 而口径本就允许多等。
    task<void> Wait() override;

    /// 恢复指定 flight 上已就绪的等待者。
    /// 【只泵一个 flight, 且必须是调用线程当前独占的那个】否则与渲染线程的 CompleteFlight
    /// 竞争。调用点固定在 BeginUpdateForFlight。
    void PumpWaitFrame(uint32_t flightIndex);

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
    render::CommandQueue* GetMainQueue() const noexcept { return _mainQueue; }
    WindowManager* GetWindowManager() const noexcept { return _windowManager; }
    /// 注入窗口系统(非拥有)。由装配阶段(ServiceRegistry / Application)调用。
    void SetWindowManager(Nullable<WindowManager*> windowManager) noexcept { _windowManager = windowManager.Get(); }
    uint32_t GetBackBufferCount() const noexcept { return _backBufferCount; }
    uint32_t GetFlightDataCount() const noexcept { return _flightDataCount; }
    uint64_t GetFrameIndex() const noexcept { return _nowFrameIndex; }
    uint32_t GetCurrentFlightIndex() const noexcept;
    std::chrono::duration<float> GetLastFrameLatency() const noexcept { return std::chrono::duration<float>{_lastFrameLatencySeconds.load(std::memory_order_relaxed)}; }
    void AdvanceFrameIndex() noexcept { ++_nowFrameIndex; }

    /// 上一帧 GPU 执行耗时(毫秒)。启用 GpuSystemDescriptor::EnableFrameProfiler 后由
    /// 内置 GpuFrameProfiler 在每帧 resolve 后更新；未启用时返回 0。
    float GetLastGpuTimeMs() const noexcept;

    UploadMemoryStats GetUploadMemoryStats() const noexcept;

private:
    friend class AppFrameContext;
    friend class Application;
    friend class WaitFrameAwaitable;

    void SubmitFrame(uint32_t flightIndex, const AppFrameSubmitDescriptor& desc);

    WaitFrameRecord* RegisterWaitFrame(stop_token stop, std::coroutine_handle<> continuation);
    void EraseWaitFrame(WaitFrameRecord* record) noexcept;
    /// 取消全部 flight 上的等待者。关停用:挂在未提交 flight 上的记录永远等不到 fence,
    /// 不取消就是协程帧连同它捕获的 GPU 对象一起泄漏。
    void CancelAllWaitFrames() noexcept;

    Application* _app;
    WindowManager* _windowManager{nullptr};
    Nullable<render::InstanceVulkan*> _vulkanInstance{nullptr};
    unique_ptr<render::DXGIFactory> _dxgiFactory;
    shared_ptr<render::Device> _device;
    render::CommandQueue* _mainQueue{nullptr};
    const uint32_t _backBufferCount;
    const uint32_t _flightDataCount;
    QueueFrameTrack _mainQueueTrack;
    /// 【必须逐个 unique_ptr, 不能是 vector<FlightSlot>】FlightSlot 内含
    /// ManualCoroutineScheduler, 挂起的协程记录里存着回指调度器的指针 (stop callback),
    /// 搬动槽位会让那些指针指向旧地址。数量构造时定下, 故间接一层无代价。
    vector<unique_ptr<FlightSlot>> _flights;
    unique_ptr<ResourceUploader> _uploader;
    unique_ptr<FrameUploadScheduler> _frameUploadScheduler;
    unique_ptr<GpuFrameProfiler> _frameProfiler;
    uint64_t _nowFrameIndex{0};
    std::atomic<float> _lastFrameLatencySeconds{0.0f};
};

template <>
struct ServiceTraits<GpuSystem> {
    using Provides = TypeList<IWaitFrameProcessor>;
    using Dependencies = TypeList<Required<WindowManager>>;
    static void Inject(GpuSystem& self, WindowManager& windows) noexcept;
    static void Unwire(GpuSystem& self) noexcept;
};

template <>
struct RuntimeTypeTrait<GpuSystem> {
    static constexpr RuntimeTypeId value{0xe7c701b1, 0xcab6, 0x4be7, 0x94, 0xec, 0xfd, 0x8c, 0x6f, 0xd4, 0xf4, 0x68};
};

}  // namespace radray
