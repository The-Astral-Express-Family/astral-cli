#pragma once

#include <string>

namespace spdlog {
class logger;
}

namespace astral::core {

// Diagnostics always go to stderr; stdout is reserved for data
// (human text or a single JSON object). See ARCHITECTURE.md section 12.
void configureLogging(const std::string& level, bool color);
spdlog::logger& logger();

} // namespace astral::core
