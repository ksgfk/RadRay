#pragma once

#include <utility>

#include <spdlog/spdlog.h>

#include <radray/types.h>

namespace radray {

// spdlog::logger& GetDefaultLogger() noexcept;

void Log(spdlog::source_loc loc, spdlog::level::level_enum lvl, spdlog::string_view_t msg) noexcept;

template <typename... Args>
void LogDebug(spdlog::format_string_t<Args...> fmt, Args&&... args) noexcept {
    Log(spdlog::source_loc{}, spdlog::level::debug, radray::format_to(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void LogInfo(spdlog::format_string_t<Args...> fmt, Args&&... args) noexcept {
    Log(spdlog::source_loc{}, spdlog::level::info, radray::format_to(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void LogWarn(spdlog::format_string_t<Args...> fmt, Args&&... args) noexcept {
    Log(spdlog::source_loc{}, spdlog::level::warn, radray::format_to(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void LogError(spdlog::format_string_t<Args...> fmt, Args&&... args) noexcept {
    Log(spdlog::source_loc{}, spdlog::level::err, radray::format_to(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void LogAbort(spdlog::format_string_t<Args...> fmt, Args&&... args) noexcept {
    Log(spdlog::source_loc{}, spdlog::level::critical, radray::format_to(fmt, std::forward<Args>(args)...));
    std::abort();
}

}  // namespace radray

#define RADRAY_DEBUG_LOG(fmt, ...) ::radray::LogDebug(fmt __VA_OPT__(, ) __VA_ARGS__)
#define RADRAY_INFO_LOG(fmt, ...) ::radray::LogInfo(fmt __VA_OPT__(, ) __VA_ARGS__)
#define RADRAY_WARN_LOG(fmt, ...) ::radray::LogWarn(fmt __VA_OPT__(, ) __VA_ARGS__)
#define RADRAY_ERR_LOG(fmt, ...) ::radray::LogError(fmt __VA_OPT__(, ) __VA_ARGS__)
#define RADRAY_ABORT(fmt, ...) ::radray::LogAbort(fmt "\n  at {}:{}" __VA_OPT__(, ) __VA_ARGS__, __FILE__, __LINE__)
#define RADRAY_ASSERT_IMPL(x, f, ...)                   \
    do {                                                \
        if (!(x)) {                                     \
            RADRAY_ABORT(f __VA_OPT__(, ) __VA_ARGS__); \
        }                                               \
    } while (false)
#if RADRAY_IS_DEBUG
#define RADRAY_ASSERT(x, f, ...) RADRAY_ASSERT_IMPL(x, f __VA_OPT__(, ) __VA_ARGS__)
#else
#define RADRAY_ASSERT(x, f, ...)
#endif
#define RADRAY_THROW(type, fmt, ...) throw type(std::format(fmt __VA_OPT__(, ) __VA_ARGS__))