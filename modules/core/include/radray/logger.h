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

void LogLoc(std::source_location loc, LogLevel lvl, fmt::string_view msg) noexcept;

void Log(LogLevel lvl, fmt::string_view msg) noexcept;

bool ShouldLogLoc(LogLevel lvl) noexcept;

bool ShouldLog(LogLevel lvl) noexcept;

void FlushLog() noexcept;

template <typename... Args>
void LogFormat(LogLevel lvl, fmt::format_string<Args...> fmt, Args&&... args) noexcept {
    if (!ShouldLog(lvl)) {
        return;
    }
    auto str = radray::format(fmt, std::forward<Args>(args)...);
    Log(lvl, str);
}

template <typename... Args>
void LogFormatLoc(std::source_location loc, LogLevel lvl, fmt::format_string<Args...> fmt, Args&&... args) noexcept {
    if (!ShouldLogLoc(lvl)) {
        return;
    }
    auto str = radray::format(fmt, std::forward<Args>(args)...);
    LogLoc(loc, lvl, str);
}

template <typename S, typename... Args>
void LogFormatSPrintf(LogLevel lvl, const S& fmt, Args&&... args) noexcept {
    if (!ShouldLog(lvl)) {
        return;
    }
    auto str = radray::sprintf(fmt, std::forward<Args>(args)...);
    Log(lvl, str);
}

template <typename... Args>
void LogFormatSPrintfLoc(std::source_location loc, LogLevel lvl, fmt::format_string<Args...> fmt, Args&&... args) noexcept {
    if (!ShouldLogLoc(lvl)) {
        return;
    }
    auto str = radray::sprintf(fmt, std::forward<Args>(args)...);
    LogLoc(loc, lvl, str);
}

template <typename... Args>
void LogDebug(fmt::format_string<Args...> fmt, Args&&... args) noexcept {
    LogFormat(LogLevel::Debug, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void LogInfo(fmt::format_string<Args...> fmt, Args&&... args) noexcept {
    LogFormat(LogLevel::Info, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void LogWarn(fmt::format_string<Args...> fmt, Args&&... args) noexcept {
    LogFormat(LogLevel::Warn, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void LogError(fmt::format_string<Args...> fmt, Args&&... args) noexcept {
    LogFormat(LogLevel::Err, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void LogAbort(std::source_location loc, fmt::format_string<Args...> fmt, Args&&... args) noexcept {
    LogFormatLoc(loc, LogLevel::Critical, fmt, std::forward<Args>(args)...);
    std::abort();
}

template <typename... Args>
void LogDebugLoc(std::source_location loc, fmt::format_string<Args...> fmt, Args&&... args) noexcept {
    LogFormatLoc(loc, LogLevel::Debug, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void LogInfoLoc(std::source_location loc, fmt::format_string<Args...> fmt, Args&&... args) noexcept {
    LogFormatLoc(loc, LogLevel::Info, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void LogWarnLoc(std::source_location loc, fmt::format_string<Args...> fmt, Args&&... args) noexcept {
    LogFormatLoc(loc, LogLevel::Warn, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void LogErrorLoc(std::source_location loc, fmt::format_string<Args...> fmt, Args&&... args) noexcept {
    LogFormatLoc(loc, LogLevel::Err, fmt, std::forward<Args>(args)...);
}

template <typename S, typename... Args>
void LogAbortSPrintf(std::source_location loc, const S& fmt, Args&&... args) noexcept {
    LogFormatSPrintfLoc(loc, LogLevel::Critical, fmt, std::forward<Args>(args)...);
    std::abort();
}

template <typename S, typename... Args>
void LogDebugSPrintfLoc(std::source_location loc, const S& fmt, Args&&... args) noexcept {
    LogFormatSPrintfLoc(loc, LogLevel::Debug, fmt, std::forward<Args>(args)...);
}

template <typename S, typename... Args>
void LogInfoSPrintfLoc(std::source_location loc, const S& fmt, Args&&... args) noexcept {
    LogFormatSPrintfLoc(loc, LogLevel::Info, fmt, std::forward<Args>(args)...);
}

template <typename S, typename... Args>
void LogWarnSPrintfLoc(std::source_location loc, const S& fmt, Args&&... args) noexcept {
    LogFormatSPrintfLoc(loc, LogLevel::Warn, fmt, std::forward<Args>(args)...);
}

template <typename S, typename... Args>
void LogErrorSPrintfLoc(std::source_location loc, const S& fmt, Args&&... args) noexcept {
    LogFormatSPrintfLoc(loc, LogLevel::Err, fmt, std::forward<Args>(args)...);
}

template <typename S, typename... Args>
void LogDebugSPrintf(const S& fmt, Args&&... args) noexcept {
    LogFormatSPrintf(LogLevel::Debug, fmt, std::forward<Args>(args)...);
}

template <typename S, typename... Args>
void LogInfoSPrintf(const S& fmt, Args&&... args) noexcept {
    LogFormatSPrintf(LogLevel::Info, fmt, std::forward<Args>(args)...);
}

template <typename S, typename... Args>
void LogWarnSPrintf(const S& fmt, Args&&... args) noexcept {
    LogFormatSPrintf(LogLevel::Warn, fmt, std::forward<Args>(args)...);
}

template <typename S, typename... Args>
void LogErrorSPrintf(const S& fmt, Args&&... args) noexcept {
    LogFormatSPrintf(LogLevel::Err, fmt, std::forward<Args>(args)...);
}

}  // namespace radray

#ifdef RADRAY_IS_DEBUG
#define RADRAY_DEBUG_LOG(fmt, ...) ::radray::LogDebug(fmt __VA_OPT__(, ) __VA_ARGS__)
#else
#define RADRAY_DEBUG_LOG(fmt, ...)
#endif
#define RADRAY_INFO_LOG(fmt, ...) ::radray::LogInfo(fmt __VA_OPT__(, ) __VA_ARGS__)
#define RADRAY_WARN_LOG(fmt, ...) ::radray::LogWarn(fmt __VA_OPT__(, ) __VA_ARGS__)
#define RADRAY_ERR_LOG(fmt, ...) ::radray::LogError(fmt __VA_OPT__(, ) __VA_ARGS__)
#define RADRAY_ABORT(fmt, ...) ::radray::LogAbort(::std::source_location::current(), fmt __VA_OPT__(, ) __VA_ARGS__)

#ifdef RADRAY_IS_DEBUG
#define RADRAY_DEBUG_LOG_CSTYLE(fmt, ...) ::radray::LogDebugSPrintf(fmt __VA_OPT__(, ) __VA_ARGS__)
#else
#define RADRAY_DEBUG_LOG_CSTYLE(fmt, ...)
#endif
#define RADRAY_INFO_LOG_CSTYLE(fmt, ...) ::radray::LogInfoSPrintf(fmt __VA_OPT__(, ) __VA_ARGS__)
#define RADRAY_WARN_LOG_CSTYLE(fmt, ...) ::radray::LogWarnSPrintf(fmt __VA_OPT__(, ) __VA_ARGS__)
#define RADRAY_ERR_LOG_CSTYLE(fmt, ...) ::radray::LogErrorSPrintf(fmt __VA_OPT__(, ) __VA_ARGS__)
#define RADRAY_ABORT_CSTYLE(fmt, ...) ::radray::LogAbortSPrintfLoc(::std::source_location::current(), fmt __VA_OPT__(, ) __VA_ARGS__)

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
