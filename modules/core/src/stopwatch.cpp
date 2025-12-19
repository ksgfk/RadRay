#include <radray/stopwatch.h>

#ifdef RADRAY_PLATFORM_WINDOWS

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WINDOWS
#define _WINDOWS
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <windows.h>

namespace radray {

namespace {

int64_t _QueryPerformanceFrequencyNow() noexcept {
    LARGE_INTEGER f{};
    if (!::QueryPerformanceFrequency(&f)) {
        return int64_t{0};
    }
    return static_cast<int64_t>(f.QuadPart);
}

int64_t _QueryPerformanceCounterNow() noexcept {
    LARGE_INTEGER t{};
    ::QueryPerformanceCounter(&t);
    return static_cast<int64_t>(t.QuadPart);
}

int64_t _Frequency() noexcept {
    return _QueryPerformanceFrequencyNow();
}

int64_t GetTimestamp() noexcept {
    return _QueryPerformanceCounterNow();
}

}  // namespace

Stopwatch::Stopwatch() noexcept { _frequency = _Frequency(); }

Stopwatch Stopwatch::StartNew() noexcept {
    Stopwatch sw{};
    sw.Start();
    return sw;
}

void Stopwatch::Start() noexcept {
    if (_isRunning) {
        return;
    }
    _startTimestamp = GetTimestamp();
    _isRunning = true;
}

void Stopwatch::Stop() noexcept {
    if (!_isRunning) {
        return;
    }
    const int64_t now = GetTimestamp();
    _elapsedTicks += (now - _startTimestamp);
    _isRunning = false;
}

void Stopwatch::Reset() noexcept {
    _elapsedTicks = 0;
    _startTimestamp = 0;
    _isRunning = false;
}

void Stopwatch::Restart() noexcept {
    Reset();
    Start();
}

bool Stopwatch::IsRunning() const noexcept { return _isRunning; }

int64_t Stopwatch::ElapsedTicksInternal(int64_t timestampNow) const noexcept {
    if (!_isRunning) {
        return _elapsedTicks;
    }
    return _elapsedTicks + (timestampNow - _startTimestamp);
}

int64_t Stopwatch::ElapsedTicks() const noexcept {
    return ElapsedTicksInternal(GetTimestamp());
}

std::chrono::nanoseconds Stopwatch::Elapsed() const noexcept {
    const int64_t freq = (_frequency > 0) ? _frequency : _Frequency();
    if (freq <= 0) {
        return std::chrono::nanoseconds{0};
    }
    const int64_t ticks = ElapsedTicks();
    const long double seconds = static_cast<long double>(ticks) / static_cast<long double>(freq);
    const long double nanos = seconds * 1'000'000'000.0L;
    return std::chrono::nanoseconds{static_cast<int64_t>(nanos)};
}

int64_t Stopwatch::ElapsedMilliseconds() const noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Elapsed()).count();
}

}  // namespace radray

#else

namespace radray {

namespace {

int64_t SteadyClockNowNs() noexcept {
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
}

constexpr int64_t SteadyClockFrequency() noexcept {
    return int64_t{1'000'000'000};
}

}  // namespace

Stopwatch::Stopwatch() noexcept { _frequency = SteadyClockFrequency(); }

Stopwatch Stopwatch::StartNew() noexcept {
    Stopwatch sw{};
    sw.Start();
    return sw;
}

void Stopwatch::Start() noexcept {
    if (_isRunning) {
        return;
    }
    _startTimestamp = SteadyClockNowNs();
    _isRunning = true;
}

void Stopwatch::Stop() noexcept {
    if (!_isRunning) {
        return;
    }
    const int64_t now = SteadyClockNowNs();
    _elapsedTicks += (now - _startTimestamp);
    _isRunning = false;
}

void Stopwatch::Reset() noexcept {
    _elapsedTicks = 0;
    _startTimestamp = 0;
    _isRunning = false;
}

void Stopwatch::Restart() noexcept {
    Reset();
    Start();
}

bool Stopwatch::IsRunning() const noexcept { return _isRunning; }

int64_t Stopwatch::ElapsedTicksInternal(int64_t timestampNow) const noexcept {
    if (!_isRunning) {
        return _elapsedTicks;
    }
    return _elapsedTicks + (timestampNow - _startTimestamp);
}

int64_t Stopwatch::ElapsedTicks() const noexcept {
    return ElapsedTicksInternal(SteadyClockNowNs());
}

std::chrono::nanoseconds Stopwatch::Elapsed() const noexcept {
    const int64_t freq = (_frequency > 0) ? _frequency : SteadyClockFrequency();
    if (freq <= 0) {
        return std::chrono::nanoseconds{0};
    }
    const int64_t ticks = ElapsedTicks();
    const long double seconds = static_cast<long double>(ticks) / static_cast<long double>(freq);
    const long double nanos = seconds * 1'000'000'000.0L;
    return std::chrono::nanoseconds{static_cast<int64_t>(nanos)};
}

int64_t Stopwatch::ElapsedMilliseconds() const noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Elapsed()).count();
}

}  // namespace radray

#endif
