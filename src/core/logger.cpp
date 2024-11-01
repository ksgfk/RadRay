#include <radray/logger.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace radray {

static spdlog::logger g_logger = []() {
    auto sink = radray::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    spdlog::logger l{"console", sink};
    l.flush_on(spdlog::level::err);
#if defined(RADRAY_IS_DEBUG)
    spdlog::level::level_enum level = spdlog::level::debug;
#else
    spdlog::level::level_enum level = spdlog::level::info;
#endif
    l.set_level(level);
    return l;
}();

static spdlog::level::level_enum _ToSpdlogLogLevel(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return spdlog::level::trace;
        case LogLevel::Debug: return spdlog::level::debug;
        case LogLevel::Info: return spdlog::level::info;
        case LogLevel::Warn: return spdlog::level::warn;
        case LogLevel::Err: return spdlog::level::err;
        case LogLevel::Critical: return spdlog::level::critical;
        default: return spdlog::level::trace;
    }
}

void Log(SourceLocation loc, LogLevel lvl, fmt::string_view msg) noexcept {
    g_logger.log(
        spdlog::source_loc{loc.filename, loc.line, loc.funcname},
        _ToSpdlogLogLevel(lvl),
        msg);
}

}  // namespace radray
