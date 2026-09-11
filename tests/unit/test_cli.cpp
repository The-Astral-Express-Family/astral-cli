#include <catch2/catch_test_macros.hpp>

#include <string>

#include <nlohmann/json.hpp>

#include "core/exit_codes.hpp"
#include "support/api_fixture.hpp"

namespace {

// runApp/RunResult 来自 tests/unit/support/api_fixture.hpp——本文件只覆盖
// 本地行为（version/usage/stub/doctor），不需要 ApiFixture 的网络桩。
using astral_test::runApp;

TEST_CASE("astral version prints human output and exits 0") {
    const auto result = runApp({"astral", "version"});
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.out.find("astral ") == 0);
    REQUIRE(result.out.find("protocol 2") != std::string::npos);
}

TEST_CASE("astral version --json pins the machine contract") {
    const auto result = runApp({"astral", "version", "--json"});
    REQUIRE(result.exitCode == 0);

    const auto payload = nlohmann::json::parse(result.out);
    REQUIRE(payload.at("name") == "astral");
    REQUIRE(payload.at("protocolVersion") == 2);
    REQUIRE(payload.contains("version"));
    REQUIRE(payload.contains("platform"));
    REQUIRE(payload.contains("git"));
}

TEST_CASE("the --version flag prints the version and exits 0") {
    const auto result = runApp({"astral", "--version"});
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.out.find("astral ") == 0);
}

TEST_CASE("unknown subcommand is a usage error (exit 2)") {
    const auto result = runApp({"astral", "definitely-not-a-command"});
    REQUIRE(result.exitCode == static_cast<int>(astral::core::ExitCode::Usage));
    REQUIRE_FALSE(result.err.empty());
}

TEST_CASE("no subcommand is a usage error (exit 2)") {
    const auto result = runApp({"astral"});
    REQUIRE(result.exitCode == static_cast<int>(astral::core::ExitCode::Usage));
}

TEST_CASE("stub commands fail with a stable JSON error code") {
    const auto result = runApp({"astral", "workspace", "list", "--json"});
    REQUIRE(result.exitCode == static_cast<int>(astral::core::ExitCode::GenericFailure));

    const auto payload = nlohmann::json::parse(result.out);
    REQUIRE(payload.at("error").at("code") == "COMMAND_NOT_IMPLEMENTED");
}

TEST_CASE("login without server argument is a usage error") {
    const auto result = runApp({"astral", "login"});
    REQUIRE(result.exitCode == static_cast<int>(astral::core::ExitCode::Usage));
}

TEST_CASE("doctor --json emits a check array") {
    const auto result = runApp({"astral", "doctor", "--json"});
    REQUIRE(result.exitCode == 0);

    const auto payload = nlohmann::json::parse(result.out);
    REQUIRE(payload.contains("checks"));
    REQUIRE(payload.at("checks").is_array());
    REQUIRE(payload.at("checks").size() >= 5);
    for (const auto& check : payload.at("checks")) {
        REQUIRE(check.contains("name"));
        REQUIRE(check.contains("status"));
    }
}
