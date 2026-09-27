#include <catch2/catch_test_macros.hpp>

#include <string>

#include "core/distribution.hpp"

using astral::core::assetNameFor;
using astral::core::assetTripletFor;
using astral::core::parseShaLine;

TEST_CASE("asset triplet covers all five target platforms") {
    REQUIRE(assetTripletFor("linux/x86_64") == std::optional<std::string>{"linux-x64"});
    REQUIRE(assetTripletFor("linux/arm64") == std::optional<std::string>{"linux-arm64"});
    REQUIRE(assetTripletFor("macos/x86_64") == std::optional<std::string>{"macos-x64"});
    REQUIRE(assetTripletFor("macos/arm64") == std::optional<std::string>{"macos-arm64"});
    REQUIRE(assetTripletFor("windows/x86_64") == std::optional<std::string>{"windows-x64"});
}

TEST_CASE("asset triplet rejects unknown platforms") {
    REQUIRE_FALSE(assetTripletFor("unknown").has_value());
    REQUIRE_FALSE(assetTripletFor("").has_value());
    REQUIRE_FALSE(assetTripletFor("windows/x86").has_value());
    REQUIRE_FALSE(assetTripletFor("linux/X86_64").has_value());
    REQUIRE_FALSE(assetTripletFor("linux/x86_64 ").has_value());
}

TEST_CASE("asset name combines stripped tag, triplet and platform archive extension") {
    REQUIRE(assetNameFor("v0.2.0", "linux/x86_64") == std::optional<std::string>{"astral-0.2.0-linux-x64.tar.gz"});
    REQUIRE(assetNameFor("v0.2.0", "windows/x86_64") == std::optional<std::string>{"astral-0.2.0-windows-x64.zip"});
    REQUIRE(assetNameFor("0.3.1", "macos/arm64") == std::optional<std::string>{"astral-0.3.1-macos-arm64.tar.gz"});
    REQUIRE(assetNameFor("v1.0.0-rc.1", "linux/arm64") == std::optional<std::string>{"astral-1.0.0-rc.1-linux-arm64.tar.gz"});
}

TEST_CASE("asset name rejects unknown platform or empty tag") {
    REQUIRE_FALSE(assetNameFor("v0.2.0", "unknown").has_value());
    REQUIRE_FALSE(assetNameFor("", "linux/x86_64").has_value());
}

namespace {

// Realistic 64-hex digest for valid-line parsing tests.
const std::string kDigest(64, 'a');

}  // namespace

TEST_CASE("sha line parses single-space hex/filename pairs") {
    const auto sum = parseShaLine(kDigest + " app.tar.gz");
    REQUIRE(sum.has_value());
    REQUIRE(sum->hex == kDigest);
    REQUIRE(sum->filename == "app.tar.gz");
}

TEST_CASE("sha line parses multiple spaces between hex and filename") {
    const auto sum = parseShaLine(kDigest + "   app.tar.gz");
    REQUIRE(sum->hex == kDigest);
    REQUIRE(sum->filename == "app.tar.gz");
}

TEST_CASE("sha line parses tab separator") {
    const auto sum = parseShaLine(kDigest + "\tapp.tar.gz");
    REQUIRE(sum->hex == kDigest);
    REQUIRE(sum->filename == "app.tar.gz");
}

TEST_CASE("sha line parses binary marker star") {
    const auto sum = parseShaLine(kDigest + " *app.tar.gz");
    REQUIRE(sum->hex == kDigest);
    REQUIRE(sum->filename == "app.tar.gz");
}

TEST_CASE("sha line parses real 64-hex digest") {
    const std::string digest(64, 'a');
    const auto sum = parseShaLine(digest + "  " + "SHA256SUMS.txt");
    REQUIRE(sum->hex == digest);
    REQUIRE(sum->filename == "SHA256SUMS.txt");
}

TEST_CASE("sha line tolerates leading whitespace before the digest") {
    const auto sum = parseShaLine("  " + kDigest + "  app.tar.gz");
    REQUIRE(sum->hex == kDigest);
    REQUIRE(sum->filename == "app.tar.gz");
}

TEST_CASE("sha line rejects lines without 64 hex chars") {
    // 63 chars: too short.
    REQUIRE_FALSE(parseShaLine(std::string(63, 'a') + "  app.tar.gz").has_value());
    // 65 chars: too long.
    REQUIRE_FALSE(parseShaLine(std::string(65, 'a') + "  app.tar.gz").has_value());
    // Non-hex characters.
    REQUIRE_FALSE(parseShaLine(std::string(32, 'a') + std::string(32, 'g') + "  app.tar.gz").has_value());
    // A 65th hex char glued to the digest: no whitespace after 64 hex, so
    // this is not a valid line (would otherwise swallow 'a' into the name).
    REQUIRE_FALSE(parseShaLine(std::string(65, 'a') + "  app.tar.gz").has_value());
    // Digest glued to the filename with no separator at all.
    REQUIRE_FALSE(parseShaLine(kDigest + "app.tar.gz").has_value());
}

TEST_CASE("sha line rejects empty filename") {
    REQUIRE_FALSE(parseShaLine(kDigest).has_value());
    REQUIRE_FALSE(parseShaLine(kDigest + "  ").has_value());
    REQUIRE_FALSE(parseShaLine(kDigest + " *").has_value());
}

TEST_CASE("sha line rejects blank lines") {
    REQUIRE_FALSE(parseShaLine("").has_value());
    REQUIRE_FALSE(parseShaLine("   ").has_value());
}
