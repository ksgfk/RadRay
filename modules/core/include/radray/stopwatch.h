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
    int64_t ElapsedTicks() const noexcept;
    std::chrono::nanoseconds Elapsed() const noexcept;

private:
    int64_t ElapsedTicksInternal(int64_t timestampNow) const noexcept;

    int64_t _elapsedTicks{0};
    int64_t _startTimestamp{0};
    int64_t _frequency{0};
    bool _isRunning{false};
};

}  // namespace radray
