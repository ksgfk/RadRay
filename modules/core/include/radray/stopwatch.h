#pragma once

#include <chrono>

namespace radray {

class Stopwatch {
public:
    void Start() noexcept;
    void Stop() noexcept;
    void Reset() noexcept;

    int64_t ElapsedMilliseconds() const noexcept;
    int64_t ElapsedNanoseconds() const noexcept;
    int64_t RunningMilliseconds() const noexcept;
    bool IsRunning() const noexcept;

private:
    std::chrono::time_point<std::chrono::high_resolution_clock> _start{};
    std::chrono::time_point<std::chrono::high_resolution_clock> _stop{};
    bool _isRunning{false};
};

}  // namespace radray
