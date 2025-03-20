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
    const char* filename;
    int line;
    const char* funcname;
};

using fmt_memory_buffer = fmt::basic_memory_buffer<char, 128, radray::allocator<char>>;

template <typename... Args>
radray::string format(fmt::format_string<Args...> fmtStr, Args&&... args) {
    fmt_memory_buffer buf{};
    fmt::format_to(std::back_inserter(buf), fmtStr, std::forward<Args>(args)...);
    return radray::string{buf.data(), buf.size()};
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
void LogAbort(SourceLocation loc, fmt::format_string<Args...> fmt, Args&&... args) noexcept {
    auto str = radray::format(fmt, std::forward<Args>(args)...);
    Log(loc, LogLevel::Critical, str);
    std::abort();
}

template <typename... Args>
void LogDebugLoc(SourceLocation loc, fmt::format_string<Args...> fmt, Args&&... args) noexcept {
    auto str = radray::format(fmt, std::forward<Args>(args)...);
    Log(loc, LogLevel::Debug, str);
}

template <typename... Args>
void LogInfoLoc(SourceLocation loc, fmt::format_string<Args...> fmt, Args&&... args) noexcept {
    auto str = radray::format(fmt, std::forward<Args>(args)...);
    Log(loc, LogLevel::Info, str);
}

template <typename... Args>
void LogWarnLoc(SourceLocation loc, fmt::format_string<Args...> fmt, Args&&... args) noexcept {
    auto str = radray::format(fmt, std::forward<Args>(args)...);
    Log(loc, LogLevel::Warn, str);
}

template <typename... Args>
void LogErrorLoc(SourceLocation loc, fmt::format_string<Args...> fmt, Args&&... args) noexcept {
    auto str = radray::format(fmt, std::forward<Args>(args)...);
    Log(loc, LogLevel::Err, str);
}

}  // namespace radray

#define RADRAY_DEBUG_LOG(fmt, ...) ::radray::LogDebug(fmt __VA_OPT__(, ) __VA_ARGS__)
#define RADRAY_INFO_LOG(fmt, ...) ::radray::LogInfo(fmt __VA_OPT__(, ) __VA_ARGS__)
#define RADRAY_WARN_LOG(fmt, ...) ::radray::LogWarn(fmt __VA_OPT__(, ) __VA_ARGS__)
#define RADRAY_ERR_LOG(fmt, ...) ::radray::LogError(fmt __VA_OPT__(, ) __VA_ARGS__)
#define RADRAY_ABORT(fmt, ...) ::radray::LogAbort({__FILE__, __LINE__, nullptr}, fmt __VA_OPT__(, ) __VA_ARGS__)
#define RADRAY_DEBUG_LOG_LOC(fmt, ...) ::radray::LogDebugLoc({__FILE__, __LINE__, nullptr}, fmt __VA_OPT__(, ) __VA_ARGS__)
#define RADRAY_INFO_LOG_LOC(fmt, ...) ::radray::LogInfoLoc({__FILE__, __LINE__, nullptr}, fmt __VA_OPT__(, ) __VA_ARGS__)
#define RADRAY_WARN_LOG_LOC(fmt, ...) ::radray::LogWarnLoc({__FILE__, __LINE__, nullptr}, fmt __VA_OPT__(, ) __VA_ARGS__)
#define RADRAY_ERR_LOG_LOC(fmt, ...) ::radray::LogErrorLoc({__FILE__, __LINE__, nullptr}, fmt __VA_OPT__(, ) __VA_ARGS__)

#define RADRAY_ASSERT(x)                              \
    do {                                              \
        if (!(x)) [[unlikely]] {                      \
            RADRAY_ABORT("Assertion failed: {}", #x); \
        }                                             \
    } while (0)

#define RADRAY_THROW(type, fmt, ...)                                    \
    do {                                                                \
        auto tmp___ = ::radray::format(fmt __VA_OPT__(, ) __VA_ARGS__); \
        throw type(tmp___.c_str());                                     \
    } while (0)
