#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace astral::core {

// Maps a buildPlatform() string (see core/version.cpp) to the release asset
// target triplet: "linux/x86_64" -> "linux-x64" and so on for the five
// supported targets; anything else -> nullopt.
std::optional<std::string> assetTripletFor(std::string_view buildPlatform);

// Builds the release asset file name: stripV(tag) + "-" + triplet +
// (windows ? ".zip" : ".tar.gz"). E.g. tag "v0.2.0" on "linux/x86_64"
// -> "astral-0.2.0-linux-x64.tar.gz". nullopt for an unknown platform or an
// empty tag.
std::optional<std::string> assetNameFor(std::string_view tag, std::string_view buildPlatform);

struct ShaSum {
    std::string hex;
    std::string filename;
};

// Parses one SHA256SUMS line: "<64 hex><space(s)><optional '*'><filename>".
// Any whitespace run separates the digest from the file name; a single '*'
// binary-mode marker right before the file name is skipped. nullopt when the
// digest is not exactly 64 hex chars or the file name is empty.
std::optional<ShaSum> parseShaLine(std::string_view line);

}  // namespace astral::core
