#pragma once

#include <utility>
#include <cassert>
#include <iterator>
#include <source_location>

#include <fmt/format.h>
#include <fmt/printf.h>

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

using fmt_memory_buffer = fmt::basic_memory_buffer<char, 128, allocator<char>>;

template <typename... Args>
string format(fmt::format_string<Args...> fmtStr, Args&&... args) noexcept {
    fmt_memory_buffer buf{};
    fmt::vformat_to(fmt::appender(buf), fmtStr, fmt::make_format_args(args...));
    return string{buf.data(), buf.size()};
}

template <typename Char>
string vsprintf(fmt::basic_string_view<Char> fmt, typename fmt::vprintf_args<Char>::type args) {
    fmt_memory_buffer buf{};
    fmt::detail::vprintf(buf, fmt, args);
    return {buf.data(), buf.size()};
}

template <typename S, typename... T, typename Char = fmt::detail::char_t<S>>
string sprintf(const S& fmt, const T&... args) {
    return radray::vsprintf(fmt::detail::to_string_view(fmt), fmt::make_format_args<fmt::basic_printf_context<Char>>(args...));
}

void Log(std::source_location loc, LogLevel lvl, fmt::string_view msg) noexcept;

bool ShouldLog(LogLevel lvl) noexcept;

template <typename... Args>
void LogAbort(std::source_location loc, fmt::format_string<Args...> fmt, Args&&... args) noexcept {
    auto str = radray::format(fmt, std::forward<Args>(args)...);
    Log(loc, LogLevel::Critical, str);
    std::abort();
}

template <typename... Args>
void LogDebugLoc(std::source_location loc, fmt::format_string<Args...> fmt, Args&&... args) noexcept {
    if (!ShouldLog(LogLevel::Debug)) {
        return;
    }
    auto str = radray::format(fmt, std::forward<Args>(args)...);
    Log(loc, LogLevel::Debug, str);
}

template <typename... Args>
void LogInfoLoc(std::source_location loc, fmt::format_string<Args...> fmt, Args&&... args) noexcept {
    if (!ShouldLog(LogLevel::Info)) {
        return;
    }
    auto str = radray::format(fmt, std::forward<Args>(args)...);
    Log(loc, LogLevel::Info, str);
}

template <typename... Args>
void LogWarnLoc(std::source_location loc, fmt::format_string<Args...> fmt, Args&&... args) noexcept {
    if (!ShouldLog(LogLevel::Warn)) {
        return;
    }
    auto str = radray::format(fmt, std::forward<Args>(args)...);
    Log(loc, LogLevel::Warn, str);
}

template <typename... Args>
void LogErrorLoc(std::source_location loc, fmt::format_string<Args...> fmt, Args&&... args) noexcept {
    if (!ShouldLog(LogLevel::Err)) {
        return;
    }
    auto str = radray::format(fmt, std::forward<Args>(args)...);
    Log(loc, LogLevel::Err, str);
}

template <typename... Args>
void LogAbortSPrintf(std::source_location loc, std::string_view fmt, Args&&... args) noexcept {
    auto str = radray::sprintf(fmt, std::forward<Args>(args)...);
    Log(loc, LogLevel::Critical, str);
    std::abort();
}

template <typename... Args>
void LogDebugSPrintf(std::source_location loc, std::string_view fmt, Args&&... args) noexcept {
    if (!ShouldLog(LogLevel::Debug)) {
        return;
    }
    auto str = radray::sprintf(fmt, std::forward<Args>(args)...);
    Log(loc, LogLevel::Debug, str);
}

template <typename... Args>
void LogInfoSPrintf(std::source_location loc, std::string_view fmt, Args&&... args) noexcept {
    if (!ShouldLog(LogLevel::Info)) {
        return;
    }
    auto str = radray::sprintf(fmt, std::forward<Args>(args)...);
    Log(loc, LogLevel::Info, str);
}

template <typename... Args>
void LogWarnSPrintf(std::source_location loc, std::string_view fmt, Args&&... args) noexcept {
    if (!ShouldLog(LogLevel::Warn)) {
        return;
    }
    auto str = radray::sprintf(fmt, std::forward<Args>(args)...);
    Log(loc, LogLevel::Warn, str);
}

template <typename... Args>
void LogErrorSPrintf(std::source_location loc, std::string_view fmt, Args&&... args) noexcept {
    if (!ShouldLog(LogLevel::Err)) {
        return;
    }
    auto str = radray::sprintf(fmt, std::forward<Args>(args)...);
    Log(loc, LogLevel::Err, str);
}

}  // namespace radray

#define RADRAY_DEBUG_LOG(fmt, ...) ::radray::LogDebugLoc(::std::source_location::current(), fmt __VA_OPT__(, ) __VA_ARGS__)
#define RADRAY_INFO_LOG(fmt, ...) ::radray::LogInfoLoc(::std::source_location::current(), fmt __VA_OPT__(, ) __VA_ARGS__)
#define RADRAY_WARN_LOG(fmt, ...) ::radray::LogWarnLoc(::std::source_location::current(), fmt __VA_OPT__(, ) __VA_ARGS__)
#define RADRAY_ERR_LOG(fmt, ...) ::radray::LogErrorLoc(::std::source_location::current(), fmt __VA_OPT__(, ) __VA_ARGS__)
#define RADRAY_ABORT(fmt, ...) ::radray::LogAbort(::std::source_location::current(), fmt __VA_OPT__(, ) __VA_ARGS__)

#define RADRAY_DEBUG_LOG_CSTYLE(fmt, ...) ::radray::LogDebugSPrintf(::std::source_location::current(), fmt __VA_OPT__(, ) __VA_ARGS__)
#define RADRAY_INFO_LOG_CSTYLE(fmt, ...) ::radray::LogInfoSPrintf(::std::source_location::current(), fmt __VA_OPT__(, ) __VA_ARGS__)
#define RADRAY_WARN_LOG_CSTYLE(fmt, ...) ::radray::LogWarnSPrintf(::std::source_location::current(), fmt __VA_OPT__(, ) __VA_ARGS__)
#define RADRAY_ERR_LOG_CSTYLE(fmt, ...) ::radray::LogErrorSPrintf(::std::source_location::current(), fmt __VA_OPT__(, ) __VA_ARGS__)
#define RADRAY_ABORT_CSTYLE(fmt, ...) ::radray::LogAbortSPrintf(::std::source_location::current(), fmt __VA_OPT__(, ) __VA_ARGS__)

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
