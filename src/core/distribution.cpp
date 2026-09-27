#include "core/distribution.hpp"

#include <algorithm>

namespace astral::core {
namespace {

std::string_view stripV(std::string_view tag) {
    if (!tag.empty() && tag.front() == 'v') {
        tag.remove_prefix(1);
    }
    return tag;
}

bool isHexChar(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool isWhitespace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

bool is64Hex(std::string_view text) {
    return text.size() == 64 &&
           std::all_of(text.begin(), text.end(), [](char c) { return isHexChar(c); });
}

}  // namespace

std::optional<std::string> assetTripletFor(std::string_view buildPlatform) {
    if (buildPlatform == "linux/x86_64") return std::string{"linux-x64"};
    if (buildPlatform == "linux/arm64") return std::string{"linux-arm64"};
    if (buildPlatform == "macos/x86_64") return std::string{"macos-x64"};
    if (buildPlatform == "macos/arm64") return std::string{"macos-arm64"};
    if (buildPlatform == "windows/x86_64") return std::string{"windows-x64"};
    return std::nullopt;
}

std::optional<std::string> assetNameFor(std::string_view tag, std::string_view buildPlatform) {
    if (tag.empty()) return std::nullopt;
    const auto triplet = assetTripletFor(buildPlatform);
    if (!triplet) return std::nullopt;
    const char* extension = buildPlatform.starts_with("windows/") ? ".zip" : ".tar.gz";
    return "astral-" + std::string(stripV(tag)) + "-" + *triplet + extension;
}

std::optional<ShaSum> parseShaLine(std::string_view line) {
    // Digest: exactly 64 hex chars starting at the first non-whitespace char.
    size_t begin = 0;
    while (begin < line.size() && isWhitespace(line[begin])) ++begin;
    if (begin + 64 > line.size() || !is64Hex(line.substr(begin, 64))) return std::nullopt;
    const std::string_view hex = line.substr(begin, 64);

    // Whitespace run separates the digest from the (optionally '*'-prefixed)
    // file name; at least one separator char is mandatory, otherwise a longer
    // hex run or a glued filename would silently mis-parse.
    size_t cursor = begin + 64;
    if (cursor >= line.size() || !isWhitespace(line[cursor])) return std::nullopt;
    while (cursor < line.size() && isWhitespace(line[cursor])) ++cursor;
    if (cursor < line.size() && line[cursor] == '*') ++cursor;  // binary-mode marker
    const std::string_view filename = line.substr(cursor);
    if (filename.empty()) return std::nullopt;
    return ShaSum{std::string(hex), std::string(filename)};
}

}  // namespace astral::core
