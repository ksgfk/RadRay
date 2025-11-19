#include <radray/logger.h>

#include <cctype>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/pattern_formatter.h>

namespace radray {

class maybe_print_source_loc_formatter : public spdlog::custom_flag_formatter {
public:
    void format(const spdlog::details::log_msg& msg, const std::tm&, spdlog::memory_buf_t& dest) override {
        std::string_view fn{msg.source.filename};
        if (fn.empty()) {
            return;
        }
        bool allWhitespace = std::all_of(fn.begin(), fn.end(), [](char c) { return std::isspace(static_cast<unsigned char>(c)); });
        if (allWhitespace) {
            return;
        }
        auto str = fmt::format(" {}:{}\n", msg.source.filename, msg.source.line);
        dest.append(str.data(), str.data() + str.size());
    }

    std::unique_ptr<custom_flag_formatter> clone() const override {
        return spdlog::details::make_unique<maybe_print_source_loc_formatter>();
    }
};

static spdlog::logger g_logger = []() {
    auto sink = make_shared<spdlog::sinks::stdout_color_sink_mt>();
    spdlog::logger l{"default", sink};
    auto formatter = make_unique<spdlog::pattern_formatter>();
    formatter->add_flag<maybe_print_source_loc_formatter>('q').set_pattern("%^[%Y-%m-%d %T.%e][%l] %q%$%v");
    l.flush_on(spdlog::level::err);
    // l.set_pattern("%^[%Y-%m-%d %T.%e][%l] %@%$ %v");
    l.set_formatter(std::move(formatter));
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

void Log(std::source_location loc, LogLevel lvl, fmt::string_view msg) noexcept {
    g_logger.log(
        spdlog::source_loc{loc.file_name(), (int)loc.line(), loc.function_name()},
        _ToSpdlogLogLevel(lvl),
        msg);
}

bool ShouldLog(LogLevel lvl) noexcept {
    return g_logger.should_log(_ToSpdlogLogLevel(lvl));
}

void FlushLog() noexcept {
    g_logger.flush();
}

}  // namespace radray
