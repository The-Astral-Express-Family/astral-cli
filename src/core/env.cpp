#include "core/env.hpp"

#include <cstdlib>

namespace astral::core::env {

std::optional<std::string> get(const std::string& name) {
    const char* value = std::getenv(name.c_str());
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::string(value);
}

} // namespace astral::core::env
