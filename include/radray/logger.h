#pragma once

#include <utility>
#include <cassert>
#include <iterator>

#include <fmt/format.h>

#include <radray/types.h>

namespace radray {

enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Err,
    Critical
};

struct SourceLocation {
    constexpr SourceLocation() = default;
    constexpr SourceLocation(const char* file, int line, const char* func)
        : filename{file},
          line{line},
          funcname{func} {}

    const char* filename{nullptr};
    int line{0};
    const char* funcname{nullptr};
};

using fmt_memory_buffer = fmt::basic_memory_buffer<char, 128, allocator<char>>;

template <typename... Args>
string format(fmt::format_string<Args...> fmtStr, Args&&... args) {
    fmt_memory_buffer buf{};
    fmt::format_to(std::back_inserter(buf), fmtStr, std::forward<Args>(args)...);
    return string{buf.data(), buf.size()};
}

void Log(SourceLocation loc, LogLevel lvl, fmt::string_view msg) noexcept;

template <typename... Args>
void LogDebug(fmt::format_string<Args...> fmt, Args&&... args) noexcept {
    auto str = radray::format(fmt, std::forward<Args>(args)...);
    Log(SourceLocation{}, LogLevel::Debug, str);
}

template <typename... Args>
void LogInfo(fmt::format_string<Args...> fmt, Args&&... args) noexcept {
    auto str = radray::format(fmt, std::forward<Args>(args)...);
    Log(SourceLocation{}, LogLevel::Info, str);
}

template <typename... Args>
void LogWarn(fmt::format_string<Args...> fmt, Args&&... args) noexcept {
    auto str = radray::format(fmt, std::forward<Args>(args)...);
    Log(SourceLocation{}, LogLevel::Warn, str);
}

template <typename... Args>
void LogError(fmt::format_string<Args...> fmt, Args&&... args) noexcept {
    auto str = radray::format(fmt, std::forward<Args>(args)...);
    Log(SourceLocation{}, LogLevel::Err, str);
}

template <typename... Args>
void LogAbort(fmt::format_string<Args...> fmt, Args&&... args) noexcept {
    auto str = radray::format(fmt, std::forward<Args>(args)...);
    Log(SourceLocation{}, LogLevel::Critical, str);
    std::abort();
}

}  // namespace radray

#define RADRAY_DEBUG_LOG(fmt, ...) ::radray::LogDebug(fmt __VA_OPT__(, ) __VA_ARGS__)
#define RADRAY_INFO_LOG(fmt, ...) ::radray::LogInfo(fmt __VA_OPT__(, ) __VA_ARGS__)
#define RADRAY_WARN_LOG(fmt, ...) ::radray::LogWarn(fmt __VA_OPT__(, ) __VA_ARGS__)
#define RADRAY_ERR_LOG(fmt, ...) ::radray::LogError(fmt __VA_OPT__(, ) __VA_ARGS__)
#define RADRAY_ABORT(fmt, ...) ::radray::LogAbort(fmt "\n  at {}:{}" __VA_OPT__(, ) __VA_ARGS__, __FILE__, __LINE__)
#if RADRAY_IS_DEBUG
#define RADRAY_ASSERT(x) assert(x)
#else
#define RADRAY_ASSERT(x)
#endif
#define RADRAY_THROW(type, fmt, ...)                                    \
    do {                                                                \
        auto tmp___ = ::radray::format(fmt __VA_OPT__(, ) __VA_ARGS__); \
        throw type(tmp___.c_str());                                     \
    } while (0)
