#include "core/log.hpp"

#include <memory>
#include <string>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>

namespace astral::core {

void configureLogging(const std::string& level, bool color) {
    std::shared_ptr<spdlog::sinks::sink> sink;
    if (color) {
        auto colorSink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
        colorSink->set_color(spdlog::level::err, "\033[1;31m"); // bold red
        colorSink->set_color(spdlog::level::warn, "\033[33m");  // yellow
        sink = std::move(colorSink);
    } else {
        sink = std::make_shared<spdlog::sinks::stderr_sink_mt>();
    }

    auto log = std::make_shared<spdlog::logger>("astral", std::move(sink));
    // spdlog spells it "warning"; the CLI accepts both "warn" and "warning".
    const std::string spdName = level == "warn" ? "warning" : level;
    log->set_level(spdlog::level::from_str(spdName));
    log->set_pattern("%Y-%m-%d %H:%M:%S.%e %^%-8l%$ %v");
    spdlog::set_default_logger(std::move(log));
    spdlog::flush_on(spdlog::level::err);
}

spdlog::logger& logger() {
    return *spdlog::default_logger_raw();
}

} // namespace astral::core
