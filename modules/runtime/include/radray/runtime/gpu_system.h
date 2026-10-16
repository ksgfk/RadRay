#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <span>
#include <thread>

#include <radray/types.h>
#include <radray/nullable.h>
#include <radray/render/rhi.h>
#include <radray/runtime/frame_timeline.h>
#include <radray/runtime/asset.h>
#include <radray/runtime/gpu_cmd_allocator.h>
#include <radray/runtime/gpu_frame_resources.h>
#include <radray/runtime/gpu_resource.h>
#include <radray/runtime/startup_result.h>

// device / queue / flight / 上传 / 帧边界等待。帧序与关停顺序: docs/architecture/frame-and-gpu.md

namespace radray::render {
class CommandBuffer;
}  // namespace radray::render

namespace radray {

class StreamingAssetRefAny;
class AppWindow;
class WindowManager;
class AppFrameContext;
class FrameTimeline;
class GpuSystem;

struct SubmittedFlight {
    uint32_t FlightIndex;
    uint64_t FrameSerial;
};

struct GpuSystemDescriptor {
    render::VulkanInstanceDescriptor VulkanInstance{};
    render::DXGIFactoryDescriptor DXGIFactory{};
    render::DeviceDescriptor Device{};
    render::RhiValidationOptions RhiValidation{};
    uint32_t MainQueueIndex{0};
    uint32_t BackBufferCount{3};
    uint32_t FlightDataCount{2};
    bool EnableFrameProfiler{true};
};

/// External device owner keeps its factory/environment alive through this host's destruction.
/// The same physical queue may serve other consumers, with serialized submission and retirement.
struct GpuSystemAttachmentDescriptor {
    shared_ptr<render::Device> Device;
    uint32_t MainQueueIndex{0};
    uint32_t BackBufferCount{3};
    uint32_t FlightDataCount{2};
    bool EnableFrameProfiler{false};
};

/// AcquireWindow 返回的仅可移动目标；必须在本次录制中随一个非空命令批次归还。
/// 析构不会取消 acquire。backbuffer 的全部访问及 →Present barrier 必须在关联批次中。
struct AppFrameOutputResult {
    bool CommandsSubmitted{false};
    render::SwapChainPresentResult Presentation;
};
struct AppFrameTarget {
    AppWindow* Window{nullptr};
    render::Texture* BackBuffer{nullptr};
    render::TextureView* BackBufferView{nullptr};
    uint32_t BackBufferIndex{0};
    /// Borrowed external surface. Independent batches still execute if only this output is hidden.
    Nullable<render::SwapChain*> ExternalSwapChain{nullptr};
    std::function<bool()> IsPresentable;
    std::function<void(render::SwapChainPresentResult)> OnPresent;
    /// Per-output submission/presentation result; final GPU completion is a separate identity event.
    std::function<void(const AppFrameOutputResult&)> OnOutput;

private:
    friend class AppFrameContext;
    friend class GpuSystem;
    render::SwapChainFrame _frame;
    std::function<render::SwapChainPresentResult(render::SwapChainFrame)> _present;
    std::function<render::TextureStates()> _initialState;
    std::function<void()> _commitPresentState;
    Nullable<GpuSystem*> _owner{nullptr};
    uint64_t _frameSerial{0};
    uint32_t _flightIndex{0};
    size_t _registrationIndex{0};
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
class GpuFlightCommandAllocator final : public IGpuCmdAllocator {
public:
    render::CommandBuffer* Allocate() override;
    void Return(std::span<render::CommandBuffer*> commands) override;
    void Discard(std::span<render::CommandBuffer*> commands) override;

private:
    friend class GpuSystem;
    friend class AppFrameContext;
    bool Initialize(render::Device* device, render::CommandQueue* queue);
    render::CommandBuffer* AllocateInternal();
    void ReturnInternal(std::span<render::CommandBuffer*> commands);
    void Register(std::span<render::CommandBuffer*> commands);
    void MarkSubmitted(std::span<render::CommandBuffer*> commands);
    void DiscardInternal(std::span<render::CommandBuffer*> commands, bool accepted);
    void ResetForReuse();
    bool HasUnregistered() const noexcept;
    bool HasOutstanding() const noexcept;

    struct Entry {
        unique_ptr<render::CommandBuffer> Commands;
        enum class State : uint8_t { Idle,
                                     Recording,
                                     Closed,
                                     Accepted,
                                     Submitted,
                                     Discarded };
        State Phase{State::Idle};
    };
    struct StorageGroup {
        // Commands are destroyed before their borrowed native storage.
        unique_ptr<render::CommandAllocator> Allocator;
        vector<Entry> Pool{};
        size_t AllocatedCount{0};
        size_t RecordingCount{0};
    };
    struct CommandLocation {
        StorageGroup* Group;
        Entry* Command;
    };
    std::optional<CommandLocation> FindAllocated(render::CommandBuffer* commands) noexcept;

    Nullable<render::Device*> _device;
    Nullable<render::CommandQueue*> _queue;
    vector<StorageGroup> _groups;
    bool _applicationRecording{false};
};

struct GpuFlightSubmitBatch {
    vector<render::CommandBuffer*> CmdBuffers;
    vector<render::Fence*> SignalFences;
    vector<uint64_t> SignalValues;
    vector<render::Fence*> WaitFences;
    vector<uint64_t> WaitValues;
    std::optional<AppFrameTarget> Target;
    bool OutputCommandsSubmitted{false};
};

struct GpuFlightAcquireRegistration {
    AppWindow* Window;
    render::Texture* BackBuffer;
    render::TextureView* BackBufferView;
    uint32_t BackBufferIndex;
    bool Returned{false};
    Nullable<render::SwapChain*> ExternalSwapChain{nullptr};
};

/// runtime 拥有的 per-flight 槽位。代表流水线一条槽位在不同阶段的完整状态，
/// 按所有权/阶段分组，跨阶段的访问时序由 ready 信号量、CPU 提交完成计数与 GPU fence 保证：
///  - 录制态：渲染线程（单线程模式即主线程）在 BeginFrameRecord→Render→
///    EndFrameRecordAndSubmit 期间独占；
///  - 保活态：GT 登记和释放 Payloads，GT 经 FrameTimeline 保留 FrameSerial，GT 匹配真实完成后清零；
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

    // —— 录制态（录制阶段由渲染线程独占）。批次顺序由登记顺序决定。
    GpuFlightCommandAllocator CommandAllocator;
    unique_ptr<GpuFrameResources> FrameResources;
    vector<GpuFlightSubmitBatch> Batches;
    vector<GpuFlightAcquireRegistration> Acquisitions;
    /// BeginUpdateForFlight 保留；GT 匹配完成并释放 Payloads 后清零，清零前不得复用槽位。
    uint64_t FrameSerial{0};
    bool Recording{false};
    bool Rendered{true};

    // —— 提交态（RT 写；runner 的 GT 观察 CPU 提交完成后，在 retire 读后清）。
    GpuFenceSignal Signal;

    /// 仅由 GT 登记和释放，绑定到本槽位的 FrameSerial。
    vector<unique_ptr<FramePayload>> Payloads;
    /// RT-created GPU ownership packages; never contain GT-only asset references.
    vector<unique_ptr<FramePayload>> GpuPayloads;
};

/// Render 回调的唯一入参，封装一帧录制 API。
/// 生命周期仅限本次 Render 调用；runtime 在 BeginFrameRecord 构造并传入。
class AppFrameContext : public IRequiredGpuWork {
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
    IGpuCmdAllocator& GetCmdAllocator() const noexcept;
    GpuFrameResources& GetGpuFrameResources() const noexcept;
    /// Registers an ordered array already closed through IGpuCmdAllocator::Return; never ends it again.
    void RegisterClosedCommandBuffers(const render::CommandQueueSubmitDescriptor& desc, std::optional<AppFrameTarget> target = std::nullopt);
    void RegisterRequired(std::span<render::CommandBuffer*> commands) override;
    template <class T>
    void RetainGpuForFrameRT(T&& payload) {
        using Value = std::remove_cvref_t<T>;
        static_assert(!std::is_same_v<Value, StreamingAssetRefAny> && !std::is_base_of_v<Asset, Value> && !requires(const Value& value) { value.AsAny(); }, "RT retention cannot contain GT asset references");
        GetRecordingFlight().GpuPayloads.push_back(make_unique<GpuFlightSlot::FramePayloadValue<Value>>(std::forward<T>(payload)));
    }

    /// 按需获取窗口呈现目标。内部 AcquireNextSwapChainFrame：
    /// RequireRecreate/RetryLater/Error/最小化 → nullopt（应用跳过该窗口）。
    /// 成功时返回持有 SwapChainFrame 的目标，不分配或选择命令。
    /// 【不录任何 barrier】backbuffer 初始翻转与 →Present 收尾全部由应用显式录。
    std::optional<AppFrameTarget> AcquireWindow(AppWindow* window);
    /// Transfers a real acquire token on the main queue. Surface/device survive the final GPU signal;
    /// callbacks survive SubmitFrame. The surface cannot be recreated while its token is owned here.
    /// Backbuffer commands belong only to this target's batch; independent resource batches are preserved.
    AppFrameTarget AdoptSwapChainFrame(render::SwapChain* chain, render::SwapChainFrame frame,
                                       std::function<bool()> isPresentable, std::function<void(render::SwapChainPresentResult)> onPresent);

    /// 绑定当前工作域的上传器；封口与真实完成回收由 frame resources 掌管。
    ResourceUploader& GetUploader() const noexcept;

    HostWriteBatch& GetHostWrites() const noexcept;

    /// 逃生舱：直接拿底层设备处理建资源、自定义 compute 和 readback。
    render::Device* GetDevice() const noexcept;
    GpuSystem* GetGpuSystem() const noexcept { return _gpuSystem; }

    /// 封口并提交已归还的批次；全部命令与 acquire 目标必须已归还，之后禁止继续录制。
    void SubmitFrame();

private:
    GpuFlightSlot& GetRecordingFlight() const noexcept;
    void RegisterBatch(const render::CommandQueueSubmitDescriptor& desc, std::optional<AppFrameTarget> target, bool closeCommands);

    GpuSystem* _gpuSystem;
    uint32_t _flightIndex;
    uint64_t _frameSerial;
    std::chrono::duration<float> _deltaTime;
    std::chrono::duration<float> _lastFrameLatency;
    bool _isInModalLoop;
};

/// runtime 侧的 GPU 设备与提交所有者。职责边界:
/// - 拥有 instance / factory / device / 主队列 / fence,以及 flight 槽位、上传器和帧 profiler。
///   帧号、完成通道和帧边界等待表属于借用的 FrameTimeline。
/// - 负责"何时画":BeginFrameRecord / EndFrameRecordAndSubmit 的录制与提交时序、
///   flight 回收与 GPU 资源生命周期兜底。
/// - 提供命令、上传与帧资源，调用方决定绘制内容。
/// 线程标签：GT = Application 所在线程；RT = 录制线程，单线程模式下与 GT 相同。
/// 所有调用都要求对象存活；跨线程访问须在构造/装配发布后、拆除前。
/// [任意线程] 只保证访问器本身可并发读取，返回对象仍遵循各自的线程契约。
class GpuSystem {
public:
    using FenceSignal = GpuFenceSignal;
    using QueueFrameTrack = GpuQueueFrameTrack;
    using FlightSlot = GpuFlightSlot;

    /// [GT] 创建设备、队列与固定数量的 flight。flight 数必须与 timeline 一致。
    /// timeline 不是本对象拥有的；等待者要在本对象销毁前取消。
    static unique_ptr<GpuSystem> TryCreate(const GpuSystemDescriptor& desc, FrameTimeline& timeline, RuntimeStartupResult& startup);
    /// [GT] Shares an existing device; owns its own flights/fence and never creates another device or environment.
    /// External factory/environment must outlive this object; caller serializes access to the shared queue.
    static unique_ptr<GpuSystem> TryAttachDevice(const GpuSystemAttachmentDescriptor& desc, FrameTimeline& timeline, RuntimeStartupResult& startup);
    GpuSystem(const GpuSystem&) = delete;
    GpuSystem(GpuSystem&&) = delete;
    GpuSystem& operator=(const GpuSystem&) = delete;
    GpuSystem& operator=(GpuSystem&&) = delete;
    /// [GT，render/GPU idle] 生产者已停止、完成消息已消费；等待者由 FrameTimeline 取消。
    ~GpuSystem() noexcept;

    /// [独立宿主，retire 阶段] 会扫描全部槽位；全部提交生产者须静止，并隔离其他 retire 与槽位复用。
    /// Application 使用只访问 CPU 已交付范围的 RetireSubmittedFlights。
    /// 收据 OnCompleted 在调用线程执行；只发布完成消息，不恢复 GT 协程。
    bool CompleteFlight(uint32_t flightIndex);
    /// [GT] Only the contiguous, submission-ordered prefix handed off by the CPU producer.
    /// Acquire the producer's completion publication first. No access to slots outside this range.
    /// Returns the GPU-completed prefix length; does not release GT owners or resume coroutines.
    size_t RetireSubmittedFlights(std::span<const SubmittedFlight> flights);
    /// [exclusive queue owner] Producers must remain quiescent until this call returns.
    /// Wait only; never retire resources or dispatch application callbacks on this thread.
    void WaitMainQueueIdle();
    /// [GT，CPU 渲染生产者已停止] 等待主队列 idle，再退休所有已提交 flight；不恢复 GT 协程。
    /// 每个已完成 flight 会先发布完成，再把等待完成标记写进当时已存在的记录。
    void WaitAndRetireFlights();
    /// [GT/RT，retire 阶段] 同 CompleteFlight 的同步前提；wait=true 只等待已提交的 fence。
    bool CompleteFlightIfReady(uint32_t flightIndex, bool wait);
    /// [GT/RT，retire 阶段] 读取须与 retire、槽位复用互斥；函数本身不加锁。
    /// 仅在该 flight 已计入 rendered 之后读，与 Submit 后发布的 release 成对。
    GpuFenceSignal GetFlightGpuSignal(uint32_t flightIndex) const noexcept;
    /// [GT] 当前 flight 已可写且尚未交给 RT；上一帧须先调用 ReleaseFrameResourcesGT。
    /// 只重置 HostWrites。等待者由 FrameTimeline::PumpWaitFrame 恢复。
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
    /// [GT] 当前 flight 已可写且 fence 已清空。检查槽位后把计时起点写入 timeline。
    std::chrono::steady_clock::time_point BeginFrameTiming(uint32_t flightIndex) noexcept;
    /// [RT] 当前 flight 已交给录制线程；同时开始应用显式上传所用的 uploader。
    /// 上一轮必须已退休；打开 frame resources epoch，返回 Render 用的帧上下文。
    AppFrameContext BeginFrameRecord(
        uint32_t flightIndex,
        std::chrono::duration<float> deltaTime,
        std::chrono::duration<float> lastFrameLatency,
        bool isInModalLoop,
        bool rendered = true);

    /// [RT] 与 BeginFrameRecord 在同一录制线程调用，当前 flight 尚未交给 retire。
    /// 检查全部归还 → uploader.EndFlight / flush → 按批次 Submit / Present → 写最终 flight.Signal。
    /// acquired 窗口已不可呈现时只跳过该目标批次，仍执行必要工作并完成交换链与 fence 收尾。
    void EndFrameRecordAndSubmit(uint32_t flightIndex);

    /// [任意线程] 只读取构造后稳定的 device 指针。
    render::Device* GetDevice() const noexcept { return _device.get(); }
    /// [任意线程] 只读取主队列指针；队列操作仍须遵守提交/等待的同步约定。
    render::CommandQueue* GetMainQueue() const noexcept { return _mainQueue; }
    render::Fence* GetMainFence() const noexcept { return _mainQueueTrack.Fence.get(); }
    /// [任意线程] 只读取装配后稳定的窗口系统指针；不得与 SetWindowManager 并发。
    WindowManager* GetWindowManager() const noexcept { return _windowManager; }
    /// [GT，装配/拆除阶段] 注入非拥有的窗口系统指针；须与所有读者隔离。
    void SetWindowManager(Nullable<WindowManager*> windowManager) noexcept { _windowManager = windowManager.Get(); }
    /// [任意线程] 构造后不变的 backbuffer 数量。
    uint32_t GetBackBufferCount() const noexcept { return _backBufferCount; }
    /// [任意线程] 构造后不变的 flight 数量。
    uint32_t GetFlightDataCount() const noexcept { return _flightDataCount; }
    /// [任意线程] 装配后稳定的时间线；不得与析构并发。
    FrameTimeline& GetFrameTimeline() const noexcept { return *_timeline; }

    /// [任意线程] 原子读取最近一次 resolve 的 GPU 耗时(毫秒)。启用 EnableFrameProfiler 后由
    /// 内置 GpuFrameProfiler 在每帧 resolve 后更新；未启用时返回 0。
    float GetLastGpuTimeMs() const noexcept;

private:
    friend class AppFrameContext;

    GpuSystem(FrameTimeline& timeline, uint32_t backBufferCount, uint32_t flightDataCount) noexcept;
    bool Initialize(const GpuSystemDescriptor& desc, RuntimeStartupResult& startup);
    bool InitializeDevice(shared_ptr<render::Device> device, uint32_t queueIndex, bool enableProfiler, RuntimeStartupResult& startup);

    /// [RT] 当前 flight 的录制线程独占调用；与 BeginFrameRecord/EndFrameRecordAndSubmit 同线程。
    void SubmitFrame(uint32_t flightIndex);

    void CheckCanRetainForFrame(uint32_t flightIndex);
    void RetireFlight(uint32_t flightIndex);

    FrameTimeline* _timeline;
    WindowManager* _windowManager{nullptr};
    Nullable<render::InstanceVulkan*> _vulkanInstance{nullptr};
    Nullable<unique_ptr<render::DXGIFactory>> _dxgiFactory;
    shared_ptr<render::Device> _device;
    render::CommandQueue* _mainQueue{nullptr};
    const uint32_t _backBufferCount;
    const uint32_t _flightDataCount;
    QueueFrameTrack _mainQueueTrack;
    /// 槽位地址在录制期间保持稳定，因此逐个分配；完成身份只由 FrameTimeline 持有。
    /// 帧等待表在 FrameTimeline，不在槽位里。
    vector<unique_ptr<FlightSlot>> _flights;
    const std::thread::id _ownerThread{std::this_thread::get_id()};
    unique_ptr<GpuFrameProfiler> _frameProfiler;
};

template <>
struct RuntimeTypeTrait<GpuSystem> {
    static constexpr RuntimeTypeId value{0xe7c701b1, 0xcab6, 0x4be7, 0x94, 0xec, 0xfd, 0x8c, 0x6f, 0xd4, 0xf4, 0x68};
};

}  // namespace radray
