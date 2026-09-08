#include "workspace/target.hpp"

#include <algorithm>
#include <cctype>

namespace astral::workspace {

namespace {

std::string toLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Splits "scheme://authority/path?query#fragment" at the path start.
// Returns {schemeAndAuthority, path}.
std::pair<std::string, std::string> splitAtPath(const std::string& url) {
    const auto authorityEnd = url.find('/', url.find("://") + 3);
    if (authorityEnd == std::string::npos) {
        return {url, ""};
    }
    return {url.substr(0, authorityEnd), url.substr(authorityEnd)};
}

} // namespace

std::optional<std::string> normalizeServerUrl(const std::string& raw) {
    const auto schemeEnd = raw.find("://");
    if (schemeEnd == std::string::npos || schemeEnd == 0) {
        return std::nullopt;
    }
    const std::string scheme = toLower(raw.substr(0, schemeEnd));
    auto [authority, pathAndRest] = splitAtPath(raw);

    const auto hostEnd = authority.find(':', schemeEnd + 3);
    const std::string host = toLower(authority.substr(
        schemeEnd + 3, hostEnd == std::string::npos ? std::string::npos : hostEnd - schemeEnd - 3));

    std::string url = scheme + "://" + host;
    if (hostEnd != std::string::npos) {
        url += authority.substr(hostEnd);
    }
    // Strip a trailing slash but keep deeper paths (servers may live on a
    // sub-path, e.g. https://example.com/astral).
    if (pathAndRest == "/") {
        pathAndRest.clear();
    }
    url += pathAndRest;
    return url;
}

TargetSpec parseTargetSpec(const std::string& input,
                           const std::optional<std::string>& explicitWorkspace) {
    TargetSpec spec;
    // 先规范化（scheme/host 小写、去根尾斜杠），保证 URL 比较与凭证主键稳定；
    // 非法 scheme 的输入原样保留，由后续 discovery 阶段报错。
    const std::string url = normalizeServerUrl(input).value_or(input);
    if (explicitWorkspace) {
        spec.fullUrl = url;
        spec.workspaceName = explicitWorkspace;
        return spec;
    }

    spec.fullUrl = url;
    auto [authority, path] = splitAtPath(url);
    // Trim slashes on both sides to find the last real segment.
    const auto firstNonSlash = path.find_first_not_of('/');
    if (firstNonSlash == std::string::npos) {
        return spec;
    }
    const auto lastNonSlash = path.find_last_not_of('/');
    if (lastNonSlash == std::string::npos) {
        return spec;
    }
    const std::string trimmed = path.substr(firstNonSlash, lastNonSlash - firstNonSlash + 1);
    if (trimmed.empty()) {
        return spec;
    }

    const auto lastSegmentStart = trimmed.find_last_of('/');
    const bool hasMultipleSegments = lastSegmentStart != std::string::npos;
    const std::string lastSegment =
        hasMultipleSegments ? trimmed.substr(lastSegmentStart + 1) : trimmed;

    // Splitting only makes sense when exactly one trailing segment exists as a
    // workspace candidate: keep the remaining path on the server URL.
    std::string remainingPath =
        hasMultipleSegments ? "/" + trimmed.substr(0, lastSegmentStart) : "/";
    if (remainingPath == "/") {
        remainingPath.clear();
    }

    spec.urlAfterSplit = authority + remainingPath;
    spec.workspaceName = lastSegment;
    return spec;
}

} // namespace astral::workspace
