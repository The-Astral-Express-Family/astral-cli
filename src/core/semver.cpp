#include "core/semver.hpp"

#include <array>
#include <charconv>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <algorithm>

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

bool isNumericIdentifier(std::string_view id) {
    if (id.empty()) {
        return false;
    }
    for (const char c : id) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

bool isLegalIdentifier(std::string_view id) {
    if (id.empty()) {
        return false;
    }
    for (const char c : id) {
        const bool legal = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                           (c >= 'A' && c <= 'Z') || c == '-';
        if (!legal) {
            return false;
        }
    }
    if (isNumericIdentifier(id) && id.size() > 1 && id.front() == '0') {
        return false; // numeric identifiers must not have leading zeros (§9.3)
    }
    return true;
}

// 拆分点分隔的 prerelease 标识符；任一标识符非法即返回 false。
std::optional<std::vector<std::string_view>> splitIdentifiers(std::string_view segment) {
    std::vector<std::string_view> ids;
    std::size_t start = 0;
    while (start <= segment.size()) {
        const std::size_t dot = segment.find('.', start);
        const std::string_view id = (dot == std::string_view::npos)
                                        ? segment.substr(start)
                                        : segment.substr(start, dot - start);
        if (!isLegalIdentifier(id)) {
            return std::nullopt;
        }
        ids.push_back(id);
        if (dot == std::string_view::npos) {
            break;
        }
        start = dot + 1;
    }
    return ids;
}

// semver §11.4：单对标识符比较。数值型 < 字母数字型；数值按数值比较，
// 字母数字按 ASCII 字典序。
std::strong_ordering compareIdentifier(std::string_view lhs, std::string_view rhs) {
    const bool lhsNum = isNumericIdentifier(lhs);
    const bool rhsNum = isNumericIdentifier(rhs);
    if (lhsNum && rhsNum) {
        int lv = 0;
        int rv = 0;
        std::from_chars(lhs.data(), lhs.data() + lhs.size(), lv);
        std::from_chars(rhs.data(), rhs.data() + rhs.size(), rv);
        return lv <=> rv;
    }
    if (lhsNum != rhsNum) {
        return lhsNum ? std::strong_ordering::less : std::strong_ordering::greater;
    }
    return lhs <=> rhs;
}

} // namespace

std::optional<SemVer> SemVer::parse(std::string_view text) {
    // Strip an optional leading "v"/"V" tag prefix.
    if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) {
        text.remove_prefix(1);
    }
    // Build metadata（"+..."）整体忽略（semver §10：不参与排序）。
    const std::size_t plus = text.find('+');
    if (plus != std::string_view::npos) {
        text = text.substr(0, plus);
    }

    // 拆出 prerelease 段（"-" 之后），主干保持 X.Y.Z。
    std::string_view prerelease;
    const std::size_t dash = text.find('-');
    if (dash != std::string_view::npos) {
        prerelease = text.substr(dash + 1);
        text = text.substr(0, dash);
    }
    if (prerelease.find('-') != std::string_view::npos && prerelease.empty()) {
        return std::nullopt; // "1.2.3-" 形态（空标识符）
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

    std::string prereleaseStr{prerelease};
    if (!prerelease.empty()) {
        // "-" 存在但标识符为空（"1.2.3-"）或非法（"1.2.3-rc..1"、"rc.01"）都在这里拒绝。
        if (!splitIdentifiers(prerelease).has_value()) {
            return std::nullopt;
        }
    } else if (dash != std::string_view::npos) {
        return std::nullopt; // "1.2.3-"：dash 后无内容
    }

    return SemVer{*major, *minor, *patch, std::move(prereleaseStr)};
}

std::strong_ordering SemVer::operator<=>(const SemVer& rhs) const {
    if (const auto cmp = major <=> rhs.major; cmp != 0) {
        return cmp;
    }
    if (const auto cmp = minor <=> rhs.minor; cmp != 0) {
        return cmp;
    }
    if (const auto cmp = patch <=> rhs.patch; cmp != 0) {
        return cmp;
    }
    if (prerelease == rhs.prerelease) {
        return std::strong_ordering::equal;
    }
    // §11.3：无 prerelease 的版本 > 有 prerelease 的版本。
    if (prerelease.empty()) {
        return std::strong_ordering::greater;
    }
    if (rhs.prerelease.empty()) {
        return std::strong_ordering::less;
    }
    // §11.4：逐标识符比较；先比完的是较小集（前缀相同则集大者大）。
    const auto lhsIds = splitIdentifiers(prerelease);
    const auto rhsIds = splitIdentifiers(rhs.prerelease);
    const std::size_t common = std::min(lhsIds->size(), rhsIds->size());
    for (std::size_t i = 0; i < common; ++i) {
        const auto cmp = compareIdentifier((*lhsIds)[i], (*rhsIds)[i]);
        if (cmp != 0) {
            return cmp;
        }
    }
    return lhsIds->size() <=> rhsIds->size();
}

} // namespace astral::core
