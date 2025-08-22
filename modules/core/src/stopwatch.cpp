#include <radray/stopwatch.h>

namespace radray {

void Stopwatch::Start() noexcept {
    _start = std::chrono::high_resolution_clock::now();
    _stop = _start;
    _isRunning = true;
}

void Stopwatch::Stop() noexcept {
    _stop = std::chrono::high_resolution_clock::now();
    _isRunning = false;
}

void Stopwatch::Reset() noexcept {
    _start = {};
    _stop = {};
    _isRunning = false;
}

uint64_t Stopwatch::ElapsedMilliseconds() const noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(_stop - _start).count();
}

uint64_t Stopwatch::RunningMilliseconds() const noexcept {
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - _start).count();
}

bool Stopwatch::IsRunning() const noexcept { return _isRunning; }

}  // namespace radray
