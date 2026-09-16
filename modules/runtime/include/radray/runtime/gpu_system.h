#pragma once

#include <atomic>
#include <chrono>
#include <limits>
#include <optional>
#include <span>

#include <radray/types.h>
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

/// AcquireWindow 成功返回的轻量视图。重量级的 SwapChainFrame / sync object
/// 留在 runtime 的 per-flight FlightSlot 里，应用只拿到 backbuffer + view。
/// 【不暴露同步对象】sync object 是提交细节，由 runtime 独占。
struct AppFrameTarget {
    AppWindow* Window{nullptr};
    render::Texture* BackBuffer{nullptr};
    render::TextureView* BackBufferView{nullptr};
    uint32_t BackBufferIndex{0};
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

struct GpuFlightAcquiredTarget {
    AppWindow* Window{nullptr};
    render::SwapChainFrame Frame;
    /// Acquire 时 Begin，只录写该 HWND current backbuffer 的 pass。Submit 时与 Present 成对 Execute。
    render::CommandBuffer* Commands{nullptr};
};

/// runtime 拥有的 per-flight 槽位。代表流水线一条槽位在不同阶段的完整状态，
/// 按所有权/阶段分三组，跨阶段的访问时序由 runner 的信号量 + retire 锁保证：
///  - 录制态：渲染线程（单线程模式即主线程）在 BeginFrameRecord→Render→
///    EndFrameRecordAndSubmit 期间独占；
///  - 计时态：游戏线程在帧开头写 FrameStartTime；
///  - 提交态:Signal 由 EndFrameRecordAndSubmit 写、retire/CompleteFlight 读后清。
struct GpuFlightSlot {
    using AcquiredTarget = GpuFlightAcquiredTarget;

    // —— 录制态（渲染线程独占）。CmdBuffer 是共享前缀（不写 flip backbuffer）；
    //    PresentCommandPool 按 acquire 的窗口复用，Targets 收集本帧窗口。
    unique_ptr<render::CommandBuffer> CmdBuffer;
    vector<unique_ptr<render::CommandBuffer>> PresentCommandPool;
    unique_ptr<ResourceUploader> Uploader;
    HostWriteBatch HostWrites;
    vector<AcquiredTarget> Targets;
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
    uint64_t FrameSerial() const noexcept;
    std::chrono::duration<float> DeltaTime() const noexcept { return _deltaTime; }
    std::chrono::duration<float> LastFrameLatency() const noexcept { return _lastFrameLatency; }
    bool IsInModalLoop() const noexcept { return _isInModalLoop; }

    /// runtime 已 Begin() 的共享 command buffer：不写 flip backbuffer 的录制落点。
    render::CommandBuffer* GetCommandBuffer() const noexcept;
    /// 若 texture 是本帧已 acquire 的 backbuffer，返回该 HWND 的 present CB；否则共享 CB。
    render::CommandBuffer* GetCommandBufferForTexture(render::Texture* texture) const noexcept;

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

    /// 提交并呈现当前帧。runtime 始终注入共享 CB、per-HWND present CB、flight batch、内部 fence
    /// 和 swapchain 同步；描述符中的附加 command buffer 与共享工作一起提交，不得写入 flip backbuffer。
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
    /// ThreadedRunner 持有 _retireMutex；单线程或全局 idle 时可在无并发的前提下直接调用。
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
    /// [GT] 当前 flight 已可写且尚未交给 RT；重置 HostWrites 并恢复该槽位的帧等待者。
    void BeginUpdateForFlight(uint32_t flightIndex);
    /// [GT] 完成批次与应用完成钩子处理结束后、派发本帧事件前调用；当前 flight 已可写。
    /// 返回 latency 起点，runner 同时用它计算相邻逻辑帧的 DeltaTime。
    std::chrono::steady_clock::time_point BeginFrameTiming(uint32_t flightIndex) noexcept;
    /// [RT] 当前 flight 已交给录制线程；同时开始应用显式上传所用的 uploader。
    /// 一帧开头：取/建该 flight 的共享 CommandBuffer 并 Begin()，清空上帧 acquire 的目标。
    /// Present command buffer 在 AcquireWindow 时 Begin。返回 Render 用的帧上下文。
    AppFrameContext BeginFrameRecord(
        uint32_t flightIndex,
        std::chrono::duration<float> deltaTime,
        std::chrono::duration<float> lastFrameLatency,
        bool isInModalLoop,
        bool rendered = true);

    /// [RT] 与 BeginFrameRecord 在同一录制线程调用，当前 flight 尚未交给 retire。
    /// 一帧收尾：uploader.EndFlight → 结束共享与 per-HWND CB → 聚合 sync object → Submit
    /// （D3D12 多 HWND 时每窗 Execute 后立刻 Present；其余一次 Submit 再 Present）
    /// （acquired 窗口已不可呈现时跳过本轮命令）→ 写 flight.Signal。
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
    void SubmitFrame(uint32_t flightIndex, const AppFrameSubmitDescriptor& desc);

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
    /// Application 是唯一消费者；只在 game thread 读取。
    UnboundedChannel<FlightCompletion> _flightCompletions;
    unique_ptr<GpuFrameProfiler> _frameProfiler;
    uint64_t _nowFrameIndex{0};
    uint64_t _nextFrameSerial{1};
    std::atomic<float> _lastFrameLatencySeconds{0.0f};
};

template <>
struct RuntimeTypeTrait<GpuSystem> {
    static constexpr RuntimeTypeId value{0xe7c701b1, 0xcab6, 0x4be7, 0x94, 0xec, 0xfd, 0x8c, 0x6f, 0xd4, 0xf4, 0x68};
};

}  // namespace radray
