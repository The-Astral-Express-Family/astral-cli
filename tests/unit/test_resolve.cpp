#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

#include "workspace/binding.hpp"
#include "workspace/resolve.hpp"

using astral::workspace::Binding;
using astral::workspace::ResolveInput;
using astral::workspace::resolveTarget;

namespace {

Binding sampleBinding() {
    return Binding{1, "srv_01", "https://astral.example.com", "ws_01", "my-repo", "my-repo"};
}

} // namespace

TEST_CASE("explicit flags win over everything") {
    ResolveInput input;
    input.flagServer = "https://flag.example.com";
    input.flagWorkspace = "flagged";
    input.localBinding = sampleBinding();
    input.envServer = "https://env.example.com";
    input.envWorkspace = "envy";

    const auto result = resolveTarget(input);
    REQUIRE(result.ok);
    REQUIRE(result.target.serverUrl == "https://flag.example.com");
    REQUIRE(result.target.source == "flag");
}

TEST_CASE("repo binding beats environment variables") {
    ResolveInput input;
    input.localBinding = sampleBinding();
    input.envServer = "https://env.example.com";
    input.envWorkspace = "envy";

    const auto result = resolveTarget(input);
    REQUIRE(result.ok);
    REQUIRE(result.target.serverId == "srv_01");
    REQUIRE(result.target.workspaceId == "ws_01");
    REQUIRE(result.target.source == "binding");
}

TEST_CASE("environment pair resolves without a binding") {
    ResolveInput input;
    input.envServer = "https://env.example.com";
    input.envWorkspace = "envy";

    const auto result = resolveTarget(input);
    REQUIRE(result.ok);
    REQUIRE(result.target.serverUrl == "https://env.example.com");
    REQUIRE(result.target.source == "env");
}

TEST_CASE("partial flag pairs are rejected, not guessed") {
    ResolveInput serverOnly;
    serverOnly.flagServer = "https://flag.example.com";
    REQUIRE_FALSE(resolveTarget(serverOnly).ok);

    ResolveInput workspaceOnly;
    workspaceOnly.flagWorkspace = "flagged";
    REQUIRE_FALSE(resolveTarget(workspaceOnly).ok);
}

TEST_CASE("nothing resolvable produces an actionable failure") {
    const auto result = resolveTarget(ResolveInput{});
    REQUIRE_FALSE(result.ok);
    REQUIRE_FALSE(result.failureReason.empty());
}
