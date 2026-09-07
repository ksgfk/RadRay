#pragma once

// flight 完成通知队列: docs/architecture/frame-and-gpu.md

#include <mutex>
#include <span>

#include <radray/types.h>

namespace radray {

struct FlightCompletion {
    uint32_t FlightIndex{0};
    bool GpuWorkCompleted{true};
};

/// 任意 retire 线程发布，game thread 排空。载体不知道谁消费。
class FlightCompletionQueue {
public:
    void Push(FlightCompletion completion);

    /// RAII 排空。已有排空在进行时 Items() 为空，新发布的项留到下一轮。
    class Drain {
    public:
        explicit Drain(FlightCompletionQueue& queue);
        ~Drain() noexcept;

        Drain(const Drain&) = delete;
        Drain& operator=(const Drain&) = delete;
        Drain(Drain&&) = delete;
        Drain& operator=(Drain&&) = delete;

        std::span<const FlightCompletion> Items() const noexcept;

    private:
        FlightCompletionQueue& _queue;
        vector<FlightCompletion> _items;
        bool _active{false};
    };

private:
    std::mutex _mutex;
    vector<FlightCompletion> _pending;
    bool _draining{false};
};

/// flight 完成的订阅方。实现方在 game thread 被调用，一次给一批。
class IFlightCompletionObserver {
public:
    virtual ~IFlightCompletionObserver() noexcept = default;
    virtual void OnFlightsComplete(std::span<const FlightCompletion> completions) noexcept = 0;
};

}  // namespace radray
