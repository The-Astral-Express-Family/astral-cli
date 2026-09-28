#include <catch2/catch_test_macros.hpp>

#include <string_view>

#include "core/semver.hpp"

using astral::core::SemVer;

TEST_CASE("semver parse accepts optional v prefix and three numeric parts") {
    const auto prefixed = SemVer::parse("v0.2.0");
    REQUIRE(prefixed.has_value());
    REQUIRE(prefixed->major == 0);
    REQUIRE(prefixed->minor == 2);
    REQUIRE(prefixed->patch == 0);

    const auto bare = SemVer::parse("1.2.3");
    REQUIRE(bare.has_value());
    REQUIRE(bare->major == 1);
    REQUIRE(bare->minor == 2);
    REQUIRE(bare->patch == 3);
}

TEST_CASE("semver comparison orders numerically, not lexically") {
    const auto lower = SemVer::parse("v0.2.0");
    const auto higher = SemVer::parse("v0.2.1");
    REQUIRE(lower.has_value());
    REQUIRE(higher.has_value());
    REQUIRE(*lower < *higher);

    const auto ten = SemVer::parse("v0.10.0");
    const auto nine = SemVer::parse("v0.9.0");
    REQUIRE(ten.has_value());
    REQUIRE(nine.has_value());
    REQUIRE(*ten > *nine);

    const auto tagged = SemVer::parse("v1.0.0");
    const auto untagged = SemVer::parse("1.0.0");
    REQUIRE(tagged.has_value());
    REQUIRE(untagged.has_value());
    REQUIRE(*tagged == *untagged);
}

TEST_CASE("semver parse rejects malformed input") {
    for (const std::string_view bad : {"", "v", "1.2", "1.2.x", "unknown"}) {
        CAPTURE(bad);
        REQUIRE_FALSE(SemVer::parse(bad).has_value());
    }
}

TEST_CASE("semver parse truncates prerelease and build suffixes") {
    const auto parsed = SemVer::parse("1.2.3-rc.1");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->major == 1);
    REQUIRE(parsed->minor == 2);
    REQUIRE(parsed->patch == 3);
}

// ---------------------------------------------------------------------------
// Prerelease semantics (full semver ordering): dev < alpha < beta < rc < final,
// numeric identifiers compare numerically, and a version WITHOUT prerelease
// always sorts AFTER the same triple WITH any prerelease.
// ---------------------------------------------------------------------------

TEST_CASE("semver parse retains the prerelease segment") {
    CHECK(SemVer::parse("v0.1.1-rc.1")->prerelease == "rc.1");
    CHECK(SemVer::parse("0.1.1-dev.3")->prerelease == "dev.3");
    CHECK(SemVer::parse("1.2.3")->prerelease.empty());
    // build metadata still ignored entirely (per semver spec §10)
    CHECK(SemVer::parse("1.2.3-rc.1+build.5")->prerelease == "rc.1");
}

TEST_CASE("semver prerelease ordering across channels") {
    // semver §11.4：字母数字标识符按 ASCII 字典序 -> alpha < beta < dev < rc。
    // （"dev 是最早阶段"是社区惯例而非 semver 语义，这里以规范为准。）
    CHECK(SemVer::parse("0.1.1-alpha.1") < SemVer::parse("0.1.1-beta.1"));
    CHECK(SemVer::parse("0.1.1-beta.9") < SemVer::parse("0.1.1-dev.1"));
    CHECK(SemVer::parse("0.1.1-dev.1") < SemVer::parse("0.1.1-rc.1"));
    CHECK(SemVer::parse("0.1.1-rc.1") < SemVer::parse("0.1.1")); // final > rc
    CHECK(SemVer::parse("0.1.1") > SemVer::parse("0.1.1-dev.1"));
}

TEST_CASE("semver prerelease numeric identifiers compare numerically") {
    CHECK(SemVer::parse("0.1.1-rc.2") > SemVer::parse("0.1.1-rc.1"));
    CHECK(SemVer::parse("0.1.1-rc.10") > SemVer::parse("0.1.1-rc.9")); // not lexically!
    CHECK(SemVer::parse("0.1.1-dev.10") > SemVer::parse("0.1.1-dev.9"));
    // numeric identifiers sort below alphanumeric ones (semver §11.4)
    CHECK(SemVer::parse("0.1.1-1") < SemVer::parse("0.1.1-alpha"));
}

TEST_CASE("semver prerelease larger set wins when prefix equal") {
    CHECK(SemVer::parse("0.1.1-rc.1.2") > SemVer::parse("0.1.1-rc.1"));
    CHECK(SemVer::parse("0.1.1-alpha") < SemVer::parse("0.1.1-alpha.1"));
}

TEST_CASE("semver invalid prereleases are rejected") {
    CHECK_FALSE(SemVer::parse("1.2.3-").has_value());      // empty identifier
    CHECK_FALSE(SemVer::parse("1.2.3-rc..1").has_value()); // empty middle identifier
    CHECK_FALSE(SemVer::parse("1.2.3-rc.01").has_value()); // leading zero in numeric identifier
    CHECK_FALSE(SemVer::parse("1.2.3-rc.1!").has_value()); // illegal character
    // build metadata after '-' only is invalid; '+' must start build section
    CHECK(SemVer::parse("1.2.3-rc.1").has_value());
}
