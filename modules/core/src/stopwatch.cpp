#include <radray/stopwatch.h>

namespace radray {

Stopwatch::Stopwatch() noexcept = default;

Stopwatch Stopwatch::StartNew() noexcept {
    Stopwatch sw{};
    sw.Start();
    return sw;
}

void Stopwatch::Start() noexcept {
    if (_isRunning) {
        return;
    }
    _startTimestamp = Clock::now();
    _isRunning = true;
}

void Stopwatch::Stop() noexcept {
    if (!_isRunning) {
        return;
    }
    const auto now = Clock::now();
    _elapsed += std::chrono::duration_cast<std::chrono::nanoseconds>(now - _startTimestamp);
    _isRunning = false;
}

void Stopwatch::Reset() noexcept {
    _elapsed = std::chrono::nanoseconds{0};
    _startTimestamp = Clock::time_point{};
    _isRunning = false;
}

void Stopwatch::Restart() noexcept {
    Reset();
    Start();
}

bool Stopwatch::IsRunning() const noexcept { return _isRunning; }

std::chrono::nanoseconds Stopwatch::Elapsed() const noexcept {
    if (!_isRunning) {
        return _elapsed;
    }
    const auto now = Clock::now();
    return _elapsed + std::chrono::duration_cast<std::chrono::nanoseconds>(now - _startTimestamp);
}

int64_t Stopwatch::ElapsedMilliseconds() const noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Elapsed()).count();
}

}  // namespace radray
