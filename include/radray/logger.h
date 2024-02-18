#pragma once

#include <cstdlib>
#include <utility>
#include <spdlog/spdlog.h>

namespace radray {

using Logger = spdlog::logger;
using LogLevel = spdlog::level::level_enum;

Logger& GetDefaultLogger() noexcept;

template <typename... Args>
void LogDebug(spdlog::format_string_t<Args...> fmt, Args&&... args) noexcept {
    GetDefaultLogger().debug(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void LogInfo(spdlog::format_string_t<Args...> fmt, Args&&... args) noexcept {
    GetDefaultLogger().info(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void LogWarn(spdlog::format_string_t<Args...> fmt, Args&&... args) noexcept {
    GetDefaultLogger().warn(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void LogError(spdlog::format_string_t<Args...> fmt, Args&&... args) noexcept {
    GetDefaultLogger().error(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void LogAbort(spdlog::format_string_t<Args...> fmt, Args&&... args) noexcept {
    GetDefaultLogger().error(fmt, std::forward<Args>(args)...);
    std::abort();
}

}  // namespace radray

#define RADRAY_LOG_DEBUG(fmt, ...) ::radray::LogDebug(FMT_STRING(fmt) __VA_OPT__(, ) __VA_ARGS__)
#define RADRAY_LOG_INFO(fmt, ...) ::radray::LogInfo(FMT_STRING(fmt) __VA_OPT__(, ) __VA_ARGS__)
#define RADRAY_LOG_WARN(fmt, ...) ::radray::LogWarn(FMT_STRING(fmt) __VA_OPT__(, ) __VA_ARGS__)
#define RADRAY_LOG_ERROR(fmt, ...) ::radray::LogError(FMT_STRING(fmt) __VA_OPT__(, ) __VA_ARGS__)

#define RADRAY_LOG_DEBUG_AT_SRC(fmt, ...) RADRAY_LOG_DEBUG("[{}:{}] " fmt, __FILE__, __LINE__ __VA_OPT__(, ) __VA_ARGS__)
#define RADRAY_LOG_INFO_AT_SRC(fmt, ...) RADRAY_LOG_INFO("[{}:{}] " fmt, __FILE__, __LINE__ __VA_OPT__(, ) __VA_ARGS__)
#define RADRAY_LOG_WARN_AT_SRC(fmt, ...) RADRAY_LOG_WARN("[{}:{}] " fmt, __FILE__, __LINE__ __VA_OPT__(, ) __VA_ARGS__)
#define RADRAY_LOG_ERROR_AT_SRC(fmt, ...) RADRAY_LOG_ERROR("[{}:{}] " fmt, __FILE__, __LINE__ __VA_OPT__(, ) __VA_ARGS__)

#define RADRAY_ABORT(fmt, ...) ::radray::LogAbort(fmt "\n  at {}:{}" __VA_OPT__(, ) __VA_ARGS__, __FILE__, __LINE__)

#define RADRAY_ASSERT(x, f, ...)                                    \
    do {                                                            \
        if (!(x)) {                                                 \
            auto msg = ::fmt::format(f __VA_OPT__(, ) __VA_ARGS__); \
            RADRAY_ABORT("assertion '{}' failed: {}", #x, msg);     \
        }                                                           \
    } while (false)
