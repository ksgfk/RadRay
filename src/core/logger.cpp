#include <radray/logger.h>

#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace radray {

static spdlog::logger g_logger = []() {
    auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
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

spdlog::logger& GetDefaultLogger() noexcept { return g_logger; }

void Log(spdlog::source_loc loc, spdlog::level::level_enum lvl, spdlog::string_view_t msg) noexcept {
    g_logger.log(loc, lvl, msg);
}

}  // namespace radray
