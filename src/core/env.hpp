#pragma once

#include <optional>
#include <string>

namespace astral::core::env {

std::optional<std::string> get(const std::string& name);

inline bool has(const std::string& name) {
    return get(name).has_value();
}

} // namespace astral::core::env
