#pragma once

#include <compare>
#include <optional>
#include <string_view>

namespace astral::core {

struct SemVer {
    int major = 0;
    int minor = 0;
    int patch = 0;

    // Accepts "v1.2.3" / "1.2.3"; requires three numeric parts and ignores any
    // trailing prerelease/build suffix (this repository does not publish those).
    // Invalid input yields std::nullopt.
    static std::optional<SemVer> parse(std::string_view text);

    std::strong_ordering operator<=>(const SemVer&) const = default;
};

} // namespace astral::core
