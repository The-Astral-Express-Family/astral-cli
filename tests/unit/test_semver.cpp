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
