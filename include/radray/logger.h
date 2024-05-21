#pragma once

#include <utility>
#include <sstream>
#include <ostream>
#include <format>
#include <string_view>

#include <spdlog/spdlog.h>

namespace radray {

spdlog::logger& GetDefaultLogger() noexcept;

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

template <typename Char>
struct OStreamFormatter : public std::formatter<std::basic_string_view<Char>, Char> {
    template <typename T, typename OutputIt>
    auto format(const T& value, std::basic_format_context<OutputIt, Char>& ctx) const -> OutputIt {
        std::basic_stringbuf<Char> buf{};
        std::basic_ostream<Char> output{&buf};
        output.imbue(ctx.locale());
        output << value;
        output.exceptions(std::ios_base::failbit | std::ios_base::badbit);
        return std::formatter<std::basic_string<Char>, Char>::format(buf.view(), ctx);
    }
};

}  // namespace radray

#define RADRAY_DEBUG_LOG(fmt, ...) ::radray::LogDebug(fmt __VA_OPT__(, ) __VA_ARGS__)
#define RADRAY_INFO_LOG(fmt, ...) ::radray::LogInfo(fmt __VA_OPT__(, ) __VA_ARGS__)
#define RADRAY_WARN_LOG(fmt, ...) ::radray::LogWarn(fmt __VA_OPT__(, ) __VA_ARGS__)
#define RADRAY_ERR_LOG(fmt, ...) ::radray::LogError(fmt __VA_OPT__(, ) __VA_ARGS__)
#define RADRAY_ABORT(fmt, ...) ::radray::LogAbort(fmt "\n  at {}:{}" __VA_OPT__(, ) __VA_ARGS__, __FILE__, __LINE__)
#define RADRAY_ASSERT_IMPL(x, f, ...)                               \
    do {                                                            \
        if (!(x)) {                                                 \
            auto msg = ::std::format(f __VA_OPT__(, ) __VA_ARGS__); \
            RADRAY_ABORT("assertion '{}' failed: {}", #x, msg);     \
        }                                                           \
    } while (false)
#if RADRAY_IS_DEBUG
#define RADRAY_ASSERT(x, f, ...) RADRAY_ASSERT_IMPL(x, f __VA_OPT__(, ) __VA_ARGS__)
#else
#define RADRAY_ASSERT(x, f, ...)
#endif
