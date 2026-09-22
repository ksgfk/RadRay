#pragma once

#include <atomic>
#include <chrono>
#include <limits>
#include <optional>
#include <span>
#include <thread>

#include <radray/types.h>
#include <radray/nullable.h>
#include <radray/channel.h>
#include <radray/coroutine.h>
#include <radray/render/rhi.h>
#include <radray/runtime/gpu_resource.h>
#include <radray/runtime/wait_frame.h>

// device / queue / flight / 上传 / 帧边界等待。帧序与关停顺序: docs/architecture/frame-and-gpu.md

namespace radray::render {
class CommandBuffer;
}  // namespace radray::render

namespace radray {

class AppWindow;
class WindowManager;
class AppFrameContext;
class WaitFrameAwaitable;
struct WaitFrameRecord;
class GpuSystem;

struct GpuSystemDescriptor {
    render::VulkanInstanceDescriptor VulkanInstance{};
    render::DXGIFactoryDescriptor DXGIFactory{};
    render::DeviceDescriptor Device{};
    uint32_t MainQueueIndex{0};
    uint32_t BackBufferCount{3};
    uint32_t FlightDataCount{2};
    bool EnableFrameProfiler{true};
};

/// 一条等待帧边界的协程记录(IWaitFrameProcessor::Wait 的挂起点)。挂在某个 flight 上,
/// 该 flight 的 fence 完成后被标记 ready,再由主线程的 PumpWaitFrame 恢复。
struct WaitFrameRecord : ManualCoroutineRecord {
    /// 记录所属的 flight。记录存在期内不变 —— 摘除时要靠它定位所在的表。
    uint32_t FlightIndex{std::numeric_limits<uint32_t>::max()};
    bool FlightComplete{false};
};

/// AcquireWindow 返回的仅可移动目标；必须在本次录制中随一个非空命令批次归还。
/// 析构不会取消 acquire。backbuffer 的全部访问及 →Present barrier 必须在关联批次中。
struct AppFrameTarget {
    AppWindow* Window{nullptr};
    render::Texture* BackBuffer{nullptr};
    render::TextureView* BackBufferView{nullptr};
    uint32_t BackBufferIndex{0};

private:
    friend class AppFrameContext;
    friend class GpuSystem;
    render::SwapChainFrame _frame;
    Nullable<GpuSystem*> _owner{nullptr};
    uint64_t _frameSerial{0};
    uint32_t _flightIndex{0};
    size_t _registrationIndex{0};
};

struct FlightCompletion {
    uint32_t FlightIndex{0};
    bool GpuWorkCompleted{true};
    uint64_t FrameSerial{0};
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

/// 当前 flight 独占的命令池；仅在上一轮退休后重置，同一轮归还的命令不会重新分配。
class GpuFlightCommandAllocator {
private:
    friend class GpuSystem;
    friend class AppFrameContext;
    render::CommandBuffer* Allocate(render::Device* device, render::CommandQueue* queue);
    void Return(std::span<render::CommandBuffer*> commands);
    void Reset();
    bool HasOutstanding() const noexcept;

    struct Entry {
        unique_ptr<render::CommandBuffer> Commands;
        bool Returned{false};
    };
    vector<Entry> _pool;
    size_t _allocatedCount{0};
};

struct GpuFlightSubmitBatch {
    vector<render::CommandBuffer*> CmdBuffers;
    vector<render::Fence*> SignalFences;
    vector<uint64_t> SignalValues;
    vector<render::Fence*> WaitFences;
    vector<uint64_t> WaitValues;
    std::optional<AppFrameTarget> Target;
};

struct GpuFlightAcquireRegistration {
    AppWindow* Window;
    render::Texture* BackBuffer;
    render::TextureView* BackBufferView;
    uint32_t BackBufferIndex;
    bool Returned{false};
};

/// runtime 拥有的 per-flight 槽位。代表流水线一条槽位在不同阶段的完整状态，
/// 按所有权/阶段分组，跨阶段的访问时序由 ready 信号量、CPU 提交完成计数与 GPU fence 保证：
///  - 录制态：渲染线程（单线程模式即主线程）在 BeginFrameRecord→Render→
///    EndFrameRecordAndSubmit 期间独占；
///  - 计时态：游戏线程在帧开头写 FrameStartTime；
///  - 保活态：GT 登记和释放 Payloads，RT 分配 FrameSerial，GT 匹配真实完成后清零；
///  - 提交态:Signal 由 EndFrameRecordAndSubmit 写、retire/CompleteFlight 读后清。
struct GpuFlightSlot {
    struct FramePayload {
        virtual ~FramePayload() noexcept = default;
    };
    template <class T>
    struct FramePayloadValue final : FramePayload {
        template <class U>
        explicit FramePayloadValue(U&& value) : Value(std::forward<U>(value)) {}
        T Value;
    };

    // —— 录制态（录制阶段由渲染线程独占）。批次顺序由归还顺序决定。
    GpuFlightCommandAllocator CommandAllocator;
    unique_ptr<ResourceUploader> Uploader;
    HostWriteBatch HostWrites;
    vector<GpuFlightSubmitBatch> Batches;
    vector<GpuFlightAcquireRegistration> Acquisitions;
    /// BeginFrameRecord 分配；GT 匹配完成并释放 Payloads 后清零，清零前不得复用槽位。
    uint64_t FrameSerial{0};
    bool Submitted{false};
    bool Recording{false};
    bool Rendered{true};

    // —— 计时态（游戏线程写）。
    std::chrono::steady_clock::time_point FrameStartTime{};

    /// 等在本 flight 帧边界上的协程 (IWaitFrameProcessor::Wait 的挂起点)。
    /// 【挂在这里而不是全局表】flight 的 fence 就是完成条件, 无需另存 fence 值再比较。
    /// CompleteFlight 只发布 WaitersCompleted；主线程 PumpWaitFrame 标记并恢复记录。
    ManualCoroutineScheduler<WaitFrameRecord> WaitFrame;
    std::atomic_bool WaitersCompleted{false};
    std::atomic<uint64_t> CompletedFrameSerial{0};

    // —— 提交态（RT 写；runner 的 GT 观察 CPU 提交完成后，在 retire 读后清）。
    GpuFenceSignal Signal;

    /// 仅由 GT 登记和释放，绑定到本槽位的 FrameSerial。
    vector<unique_ptr<FramePayload>> Payloads;
};

/// co_await GpuSystem::Wait() 的 awaitable。恢复点在 GpuSystem::PumpWaitFrame(主线程)。
class WaitFrameAwaitable {
public:
    WaitFrameAwaitable(GpuSystem* gpuSystem, stop_token stop) noexcept
        : _gpuSystem(gpuSystem), _stop(stop) {}

    bool await_ready() const noexcept;
    bool await_suspend(std::coroutine_handle<> h);
    bool await_resume() noexcept;

private:
    GpuSystem* _gpuSystem;
    stop_token _stop;
    WaitFrameRecord* _record{nullptr};
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
        bool isInModalLoop) noexcept;

    uint32_t FlightIndex() const noexcept { return _flightIndex; }
    uint64_t FrameSerial() const noexcept;
    std::chrono::duration<float> DeltaTime() const noexcept { return _deltaTime; }
    std::chrono::duration<float> LastFrameLatency() const noexcept { return _lastFrameLatency; }
    bool IsInModalLoop() const noexcept { return _isInModalLoop; }

    /// 分配当前 flight 的命令并 Begin；必须归还，调用方不得自行 End。
    render::CommandBuffer* AllocateCommandBuffer();
    /// 复制 desc 的数组、End 命令并按调用顺序登记。归还后不得修改或重复归还命令。
    /// 仅接受当前 flight 分配的命令；WaitToExecute/ReadyToPresent 必须为空。
    /// target 移交呈现所有权；其 backbuffer 的全部访问必须位于此非空批次中。
    void ReturnCommandBuffers(const render::CommandQueueSubmitDescriptor& desc, std::optional<AppFrameTarget> target = std::nullopt);

    /// 按需获取窗口呈现目标。内部 AcquireNextSwapChainFrame：
    /// RequireRecreate/RetryLater/Error/最小化 → nullopt（应用跳过该窗口）。
    /// 成功时返回持有 SwapChainFrame 的目标，不分配或选择命令。
    /// 【不录任何 barrier】backbuffer 初始翻转与 →Present 收尾全部由应用显式录。
    std::optional<AppFrameTarget> AcquireWindow(AppWindow* window);

    /// 绑定当前 flight 的上传器；EndFlight/CollectFlight 完全由 runtime 掌管。
    ResourceUploader& GetUploader() const noexcept;

    HostWriteBatch& GetHostWrites() const noexcept;

    /// 逃生舱：直接拿底层设备处理建资源、自定义 compute 和 readback。
    render::Device* GetDevice() const noexcept;
    GpuSystem* GetGpuSystem() const noexcept { return _gpuSystem; }

    /// 封口并提交已归还的批次；全部命令与 acquire 目标必须已归还，之后禁止继续录制。
    void SubmitFrame();

private:
    GpuFlightSlot& GetRecordingFlight() const noexcept;

    GpuSystem* _gpuSystem;
    uint32_t _flightIndex;
    uint64_t _frameSerial;
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
/// 线程标签：GT = Application 所在线程；RT = 录制线程，单线程模式下与 GT 相同。
/// 所有调用都要求对象存活；跨线程访问须在构造/装配发布后、拆除前。
/// [任意线程] 只保证访问器本身可并发读取，返回对象仍遵循各自的线程契约。
class GpuSystem : public IWaitFrameProcessor {
public:
    using FenceSignal = GpuFenceSignal;
    using QueueFrameTrack = GpuQueueFrameTrack;
    using FlightSlot = GpuFlightSlot;

    /// [GT] 创建设备、队列与固定数量的 flight；构造完成后才能交给 RT。
    GpuSystem(const GpuSystemDescriptor& desc);
    GpuSystem(const GpuSystem&) = delete;
    GpuSystem(GpuSystem&&) = delete;
    GpuSystem& operator=(const GpuSystem&) = delete;
    GpuSystem& operator=(GpuSystem&&) = delete;
    /// [GT，render/GPU idle] 生产者已停止、完成消息已消费；取消等待者后拆除设备。
    ~GpuSystem() noexcept;

    /// [GT] 协程的启动、恢复与取消均在 GT；当前 flight 必须由 GT 持有。
    /// IWaitFrameProcessor。挂进【当前】flight 的等待表 —— 调用点在帧顶 Update 期间,
    /// 此刻"已录制的 work"全属于更早的 flight, 故等当前 flight 的 fence 必然够。
    /// 代价是最多多等一轮, 而口径本就允许多等。
    task<void> Wait() override;

    /// [GT] 恢复指定 flight 上已就绪的等待者；关停时须先达到 render/GPU idle。
    /// 【只泵一个 flight, 且必须是调用线程当前独占的那个】否则与渲染线程的 CompleteFlight
    /// 竞争。由 BeginUpdateForFlight 或关停的 CleanupCompletedFlights 调用。
    void PumpWaitFrame(uint32_t flightIndex);

    /// [GT/RT，retire 阶段] 调用方须串行化 retire；对应 Submit 已发布，槽位尚未复用。
    /// Application runner 由 GT 独占 retire；独立驱动须自行隔离 Submit、其他 retire 与槽位复用。
    /// 收据 OnCompleted 在调用线程执行；只发布完成消息，不恢复 GT 协程。
    bool CompleteFlight(uint32_t flightIndex);
    /// [GT，CPU 渲染生产者已停止] 等待主队列 idle，再退休所有已提交 flight；不恢复 GT 协程。
    void WaitAndRetireFlights();
    /// [GT，render/GPU idle] Application 在 WaitAndRetireFlights 后调用，恢复全部 flight 的等待者。
    void CleanupCompletedFlights();
    /// [GT/RT，retire 阶段] 同 CompleteFlight 的同步前提；wait=true 只等待已提交的 fence。
    bool CompleteFlightIfReady(uint32_t flightIndex, bool wait);
    /// [GT/RT，retire 阶段] 读取须与 retire、槽位复用互斥；函数本身不加锁。
    /// 仅在该 flight 已计入 rendered 之后读，与 Submit 后发布的 release 成对。
    GpuFenceSignal GetFlightGpuSignal(uint32_t flightIndex) const noexcept;
    /// [GT] 当前 flight 已可写且尚未交给 RT；上一帧须先调用 ReleaseFrameResourcesGT。
    /// 重置 HostWrites 并恢复该槽位的帧等待者。
    void BeginUpdateForFlight(uint32_t flightIndex);
    /// GT: retain an asset owner or raw RHI payload in the writable flight before publication.
    /// Cancellation of a user task never releases this framework-owned record.
    template <class T>
    void RetainForFrameGT(uint32_t flightIndex, T&& payload) {
        CheckCanRetainForFrame(flightIndex);
        using Value = std::remove_cvref_t<T>;
        _flights[flightIndex]->Payloads.push_back(make_unique<FlightSlot::FramePayloadValue<Value>>(std::forward<T>(payload)));
    }
    /// [GT] 匹配真实完成，释放 Payloads 并清零 FrameSerial；空帧也须退休，且每帧只能调用一次。
    void ReleaseFrameResourcesGT(const FlightCompletion& completion);
    /// Terminal only: CPU producers stopped, submitted work completed; forbids future retention.
    void AbandonUnpublishedResourcesTerminalGT();
    /// [GT] 完成批次与应用完成钩子处理结束后、派发本帧事件前调用；当前 flight 已可写。
    /// 返回 latency 起点，runner 同时用它计算相邻逻辑帧的 DeltaTime。
    std::chrono::steady_clock::time_point BeginFrameTiming(uint32_t flightIndex) noexcept;
    /// [RT] 当前 flight 已交给录制线程；同时开始应用显式上传所用的 uploader。
    /// 上一轮必须已退休；重置命令分配器、批次与 acquire 登记，返回 Render 用的帧上下文。
    AppFrameContext BeginFrameRecord(
        uint32_t flightIndex,
        std::chrono::duration<float> deltaTime,
        std::chrono::duration<float> lastFrameLatency,
        bool isInModalLoop,
        bool rendered = true);

    /// [RT] 与 BeginFrameRecord 在同一录制线程调用，当前 flight 尚未交给 retire。
    /// 检查全部归还 → uploader.EndFlight / flush → 按批次 Submit / Present → 写最终 flight.Signal。
    /// acquired 窗口已不可呈现时跳过全部应用命令，仍完成交换链与 fence 收尾。
    void EndFrameRecordAndSubmit(uint32_t flightIndex);

    /// [任意线程] 只读取构造后稳定的 device 指针。
    render::Device* GetDevice() const noexcept { return _device.get(); }
    /// [任意线程] 只读取主队列指针；队列操作仍须遵守提交/等待的同步约定。
    render::CommandQueue* GetMainQueue() const noexcept { return _mainQueue; }
    /// [任意线程] 只读取装配后稳定的窗口系统指针；不得与 SetWindowManager 并发。
    WindowManager* GetWindowManager() const noexcept { return _windowManager; }
    /// [GT，装配/拆除阶段] 注入非拥有的窗口系统指针；须与所有读者隔离。
    void SetWindowManager(Nullable<WindowManager*> windowManager) noexcept { _windowManager = windowManager.Get(); }
    /// [任意线程] 构造后不变的 backbuffer 数量。
    uint32_t GetBackBufferCount() const noexcept { return _backBufferCount; }
    /// [任意线程] 构造后不变的 flight 数量。
    uint32_t GetFlightDataCount() const noexcept { return _flightDataCount; }
    /// [GT] 非原子的游戏帧号；RT 使用 runner 自己的帧号或 AppFrameContext。
    uint64_t GetFrameIndex() const noexcept { return _nowFrameIndex; }
    /// [GT] 从非原子的游戏帧号计算当前槽位；RT 使用 AppFrameContext::FlightIndex。
    uint32_t GetCurrentFlightIndex() const noexcept;
    /// [任意线程] 原子读取最近一次 retire 发布的延迟，不代表当前录制帧。
    std::chrono::duration<float> GetLastFrameLatency() const noexcept { return std::chrono::duration<float>{_lastFrameLatencySeconds.load(std::memory_order_relaxed)}; }
    /// [GT] runner 推进游戏帧号；不能与 GetFrameIndex/GetCurrentFlightIndex 跨线程并发。
    void AdvanceFrameIndex() noexcept { ++_nowFrameIndex; }

    /// [任意线程] 原子读取最近一次 resolve 的 GPU 耗时(毫秒)。启用 EnableFrameProfiler 后由
    /// 内置 GpuFrameProfiler 在每帧 resolve 后更新；未启用时返回 0。
    float GetLastGpuTimeMs() const noexcept;

private:
    friend class Application;
    friend class AppFrameContext;
    friend class WaitFrameAwaitable;

    /// [RT] 当前 flight 的录制线程独占调用；与 BeginFrameRecord/EndFrameRecordAndSubmit 同线程。
    void SubmitFrame(uint32_t flightIndex);

    /// [GT] 向当前由 GT 持有的 flight 登记等待；包含协程首次挂起路径。
    WaitFrameRecord* RegisterWaitFrame(stop_token stop, std::coroutine_handle<> continuation);
    /// [GT] 摘除等待记录；协程恢复和取消路径也必须在 GT。
    void EraseWaitFrame(WaitFrameRecord* record) noexcept;
    /// [GT，render/GPU idle] 取消并就地恢复全部 flight 的等待者。
    /// 关停用:挂在未提交 flight 上的记录永远等不到 fence,
    /// 不取消就是协程帧连同它捕获的 GPU 对象一起泄漏。
    void CancelAllWaitFrames() noexcept;
    /// [GT，独占对应 flight] 消费完成标记，只标记现有等待记录，不恢复协程。
    void MarkCompletedWaitFrames(uint32_t flightIndex) noexcept;
    void CheckCanRetainForFrame(uint32_t flightIndex) const;

    WindowManager* _windowManager{nullptr};
    Nullable<render::InstanceVulkan*> _vulkanInstance{nullptr};
    Nullable<unique_ptr<render::DXGIFactory>> _dxgiFactory;
    shared_ptr<render::Device> _device;
    render::CommandQueue* _mainQueue{nullptr};
    const uint32_t _backBufferCount;
    const uint32_t _flightDataCount;
    QueueFrameTrack _mainQueueTrack;
    /// 【必须逐个 unique_ptr, 不能是 vector<FlightSlot>】FlightSlot 内含
    /// ManualCoroutineScheduler, 挂起的协程记录里存着回指调度器的指针 (stop callback),
    /// 搬动槽位会让那些指针指向旧地址。数量构造时定下, 故间接一层无代价。
    vector<unique_ptr<FlightSlot>> _flights;
    const std::thread::id _ownerThread{std::this_thread::get_id()};
    bool _retirementStopping{false};
    /// Application 是唯一消费者；只在 game thread 读取。
    UnboundedChannel<FlightCompletion> _flightCompletions;
    unique_ptr<GpuFrameProfiler> _frameProfiler;
    uint64_t _nowFrameIndex{0};
    uint64_t _nextFrameSerial{1};
    bool _pumpingWaiters{false};
    vector<uint64_t> _waitDispatchBoundaries;
    std::atomic<float> _lastFrameLatencySeconds{0.0f};
};

template <>
struct RuntimeTypeTrait<GpuSystem> {
    static constexpr RuntimeTypeId value{0xe7c701b1, 0xcab6, 0x4be7, 0x94, 0xec, 0xfd, 0x8c, 0x6f, 0xd4, 0xf4, 0x68};
};

}  // namespace radray
