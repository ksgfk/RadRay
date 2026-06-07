#pragma once

#include <chrono>
#include <cstdint>

namespace radray {

class Stopwatch {
public:
    Stopwatch() noexcept;

    static Stopwatch StartNew() noexcept;

    void Start() noexcept;
    void Stop() noexcept;
    void Reset() noexcept;
    void Restart() noexcept;

    bool IsRunning() const noexcept;
    int64_t ElapsedMilliseconds() const noexcept;
    std::chrono::nanoseconds Elapsed() const noexcept;

private:
    using Clock = std::chrono::steady_clock;

    std::chrono::nanoseconds _elapsed{0};
    Clock::time_point _startTimestamp{};
    bool _isRunning{false};
};

}  // namespace radray
