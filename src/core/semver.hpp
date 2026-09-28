#pragma once

#include <compare>
#include <optional>
#include <string>
#include <string_view>

namespace astral::core {

struct SemVer {
    int major = 0;
    int minor = 0;
    int patch = 0;
    // Prerelease segment without the leading '-' ("rc.1", "dev.3", ""); build
    // metadata (+...) is ignored entirely per semver spec §10.
    std::string prerelease;

    // Accepts "v1.2.3" / "1.2.3" with an optional prerelease suffix
    // ("-dev.1", "-alpha.2", "-rc.1" ...) and optional "+build" metadata.
    // Identifiers must be [0-9A-Za-z-] with no leading zeros on numerics.
    // Invalid input yields std::nullopt.
    static std::optional<SemVer> parse(std::string_view text);

    // Full semver ordering: triple first; then prerelease rules (§11):
    // no-prerelease > any prerelease; identifiers compared per semver rules
    // (numeric < alphanumeric, numeric compares numerically, larger set wins).
    std::strong_ordering operator<=>(const SemVer&) const;

    bool operator==(const SemVer&) const = default;
};

} // namespace astral::core
