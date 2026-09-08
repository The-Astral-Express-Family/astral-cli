#include <catch2/catch_test_macros.hpp>

#include <optional>

#include "workspace/target.hpp"

using astral::workspace::normalizeServerUrl;
using astral::workspace::parseTargetSpec;

TEST_CASE("server/workspace shorthand splits one trailing segment") {
    const auto spec = parseTargetSpec("https://astral.example.com/my-repo", std::nullopt);
    REQUIRE(spec.fullUrl == "https://astral.example.com/my-repo");
    REQUIRE(spec.urlAfterSplit == std::optional<std::string>{"https://astral.example.com"});
    REQUIRE(spec.workspaceName == std::optional<std::string>{"my-repo"});
}

TEST_CASE("bare server URL has nothing to split") {
    const auto spec = parseTargetSpec("https://astral.example.com", std::nullopt);
    REQUIRE(spec.fullUrl == "https://astral.example.com");
    REQUIRE_FALSE(spec.urlAfterSplit.has_value());
    REQUIRE_FALSE(spec.workspaceName.has_value());
}

TEST_CASE("explicit --workspace disables shorthand splitting") {
    const auto spec =
        parseTargetSpec("https://example.com/astral", std::optional<std::string>{"my-repo"});
    REQUIRE(spec.fullUrl == "https://example.com/astral");
    REQUIRE(spec.workspaceName == std::optional<std::string>{"my-repo"});
    REQUIRE_FALSE(spec.urlAfterSplit.has_value());
}

TEST_CASE("servers on a sub-path keep the prefix when splitting") {
    const auto spec = parseTargetSpec("https://example.com/astral/my-repo", std::nullopt);
    REQUIRE(spec.urlAfterSplit == std::optional<std::string>{"https://example.com/astral"});
    REQUIRE(spec.workspaceName == std::optional<std::string>{"my-repo"});
}

TEST_CASE("trailing slashes on the shorthand are tolerated") {
    const auto spec = parseTargetSpec("https://astral.example.com/my-repo/", std::nullopt);
    REQUIRE(spec.urlAfterSplit == std::optional<std::string>{"https://astral.example.com"});
    REQUIRE(spec.workspaceName == std::optional<std::string>{"my-repo"});
}

TEST_CASE("normalizeServerUrl lowercases scheme and host, drops trailing slash") {
    REQUIRE(normalizeServerUrl("HTTPS://Astral.Example.COM/") ==
            std::optional<std::string>{"https://astral.example.com"});
    REQUIRE(normalizeServerUrl("https://astral.example.com") ==
            std::optional<std::string>{"https://astral.example.com"});
    REQUIRE(normalizeServerUrl("https://astral.example.com:8443/api") ==
            std::optional<std::string>{"https://astral.example.com:8443/api"});
}

TEST_CASE("normalizeServerUrl rejects scheme-less input") {
    REQUIRE_FALSE(normalizeServerUrl("astral.example.com").has_value());
    REQUIRE_FALSE(normalizeServerUrl("localhost:8080").has_value());
}
