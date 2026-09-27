#include "core/semver.hpp"

#include <array>
#include <charconv>
#include <string_view>
#include <system_error>

namespace astral::core {
namespace {

std::optional<int> parsePart(std::string_view part) {
    if (part.empty()) {
        return std::nullopt;
    }
    int value = 0;
    const char* const begin = part.data();
    const char* const end = begin + part.size();
    const std::from_chars_result result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
        return std::nullopt;
    }
    return value;
}

} // namespace

std::optional<SemVer> SemVer::parse(std::string_view text) {
    // Strip an optional leading "v"/"V" tag prefix.
    if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) {
        text.remove_prefix(1);
    }
    // Ignore any trailing prerelease/build suffix ("1.2.3-rc.1+build" etc.).
    const std::size_t suffix = text.find_first_of("-+");
    if (suffix != std::string_view::npos) {
        text = text.substr(0, suffix);
    }

    std::array<std::string_view, 3> parts{};
    std::size_t count = 0;
    std::size_t start = 0;
    while (start <= text.size()) {
        if (count == parts.size()) {
            return std::nullopt; // more than three parts
        }
        const std::size_t dot = text.find('.', start);
        const std::string_view part =
            (dot == std::string_view::npos) ? text.substr(start) : text.substr(start, dot - start);
        parts[count++] = part;
        if (dot == std::string_view::npos) {
            break;
        }
        start = dot + 1;
    }
    if (count != parts.size()) {
        return std::nullopt; // fewer than three parts
    }

    const auto major = parsePart(parts[0]);
    const auto minor = parsePart(parts[1]);
    const auto patch = parsePart(parts[2]);
    if (!major.has_value() || !minor.has_value() || !patch.has_value()) {
        return std::nullopt;
    }
    return SemVer{*major, *minor, *patch};
}

} // namespace astral::core
