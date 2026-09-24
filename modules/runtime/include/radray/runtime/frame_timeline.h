#pragma once

#include <atomic>
#include <chrono>
#include <limits>

#include <radray/channel.h>
#include <radray/coroutine.h>
#include <radray/runtime/wait_frame.h>
#include <radray/types.h>

// 帧号、完成消息与帧边界等待。帧序与关停顺序: docs/architecture/frame-and-gpu.md

namespace radray {

struct FlightCompletion {
    uint32_t FlightIndex{0};
    bool GpuWorkCompleted{true};
    uint64_t FrameSerial{0};
};

/// 一条等待帧边界的协程记录（IWaitFrameProcessor::Wait 的挂起点）。挂在某个 flight 上，
/// 该 flight 的完成发布后被标记 ready，再由主线程的 PumpWaitFrame 恢复。
struct WaitFrameRecord : ManualCoroutineRecord {
    /// 记录所属的 flight。记录存在期内不变 —— 摘除时要靠它定位所在的表。
    uint32_t FlightIndex{std::numeric_limits<uint32_t>::max()};
    bool FlightComplete{false};
};

class FrameTimeline;

/// co_await FrameTimeline::Wait() 的 awaitable。恢复点在 FrameTimeline::PumpWaitFrame（主线程）。
class WaitFrameAwaitable {
public:
    WaitFrameAwaitable(FrameTimeline* timeline, stop_token stop) noexcept
        : _timeline(timeline), _stop(stop) {}

    bool await_ready() const noexcept;
    bool await_suspend(std::coroutine_handle<> continuation);
    bool await_resume() noexcept;

private:
    FrameTimeline* _timeline;
    stop_token _stop;
    WaitFrameRecord* _record{nullptr};
};

/// 有无 GPU 都存在的帧时间线。拥有游戏帧号、FrameSerial、完成通道、每 flight 等待表和帧延迟。
/// GT 构造并独占帧号与等待表；PublishCompletion 可在 retire 阶段由 GT 或 RT 调用。
/// 析构会取消尚未恢复的等待者。等待者若持有 GPU 资源，必须在对应 GpuSystem 销毁前析构本对象。
class FrameTimeline : public IWaitFrameProcessor {
public:
    explicit FrameTimeline(uint32_t flightCount);
    FrameTimeline(const FrameTimeline&) = delete;
    FrameTimeline(FrameTimeline&&) = delete;
    FrameTimeline& operator=(const FrameTimeline&) = delete;
    FrameTimeline& operator=(FrameTimeline&&) = delete;
    ~FrameTimeline() noexcept;

    /// [GT] 挂进当前 flight 的等待表。调用点在帧顶 Update 期间，已发布的完成都属于更早的 flight。
    /// 停止后不再挂起。取消不是完成证据。
    task<void> Wait() override;

    /// [GT] 恢复指定 flight 上已就绪的等待者。正常只泵当前独占 flight；关停排空泵全部 flight。
    void PumpWaitFrame(uint32_t flightIndex);
    /// [GT] 先消费各 flight 已发布的完成标记，再按调用前的序号截止恢复全部等待者。
    void CleanupCompletedFlights();
    /// [GT] 把已发布的 WaitersCompleted 写进现有等待记录，不恢复协程。
    /// 窗口操作新建 Wait 之前必须先调用，避免新等待消费上一轮完成。
    void MarkCompletedWaitFrames(uint32_t flightIndex) noexcept;

    /// [GT/RT，retire 阶段] 写入完成通道、更新延迟并发布该 flight 的等待完成标记。不恢复协程。
    void PublishCompletion(const FlightCompletion& completion);
    /// [录制线程] 分配下一次录制的 FrameSerial，从 1 递增。CPU 与 GPU 共用同一计数。
    uint64_t AllocateFrameSerial();

    /// [GT] 当前 flight 已可写。返回值同时作为本帧 DeltaTime 的起点。
    std::chrono::steady_clock::time_point BeginFrameTiming(uint32_t flightIndex) noexcept;

    /// [任意线程] 构造后不变。
    uint32_t GetFlightDataCount() const noexcept { return _flightDataCount; }
    /// [GT] 非原子的游戏帧号。
    uint64_t GetFrameIndex() const noexcept { return _nowFrameIndex; }
    /// [GT] 从游戏帧号计算当前槽位。
    uint32_t GetCurrentFlightIndex() const noexcept;
    /// [GT] 推进游戏帧号。
    void AdvanceFrameIndex() noexcept { ++_nowFrameIndex; }
    /// [任意线程] 最近一次 PublishCompletion 发布的延迟。
    std::chrono::duration<float> GetLastFrameLatency() const noexcept {
        return std::chrono::duration<float>{_lastFrameLatencySeconds.load(std::memory_order_relaxed)};
    }

    /// [GT] 拒绝新的 Wait 挂起。已挂起的记录留到 Pump 或析构取消。可重复调用。
    void BeginStopping() noexcept { _stopping = true; }
    bool IsStopping() const noexcept { return _stopping; }

    /// [GT] Application 是完成通道的唯一消费者。
    bool TryReadCompletion(FlightCompletion& out) { return _completions.TryRead(out); }

private:
    friend class WaitFrameAwaitable;

    struct Flight {
        ManualCoroutineScheduler<WaitFrameRecord> Waiters;
        std::atomic_bool WaitersCompleted{false};
        std::chrono::steady_clock::time_point FrameStartTime{};
    };

    WaitFrameRecord* RegisterWaitFrame(stop_token stop, std::coroutine_handle<> continuation);
    void EraseWaitFrame(WaitFrameRecord* record) noexcept;
    void CancelAllWaitFrames() noexcept;

    /// 等待表不可移动：挂起记录里的 stop callback 回指调度器。
    vector<unique_ptr<Flight>> _flights;
    const uint32_t _flightDataCount;
    UnboundedChannel<FlightCompletion> _completions;
    uint64_t _nowFrameIndex{0};
    uint64_t _nextFrameSerial{1};
    bool _pumpingWaiters{false};
    bool _stopping{false};
    vector<uint64_t> _waitDispatchBoundaries;
    std::atomic<float> _lastFrameLatencySeconds{0.0f};
};

}  // namespace radray
