// astral profile 命令族的 runApp 级单测：HTTP 传输注入脚本化 fake，ASTRAL_HOME
// 与工作目录指向临时目录（隔离凭证文件与 .astral 绑定）。认证默认走
// ASTRAL_TOKEN（agent 自管资料的正常路径），whoami 部分用临时会话覆盖 human
// 路径。不触网。
#include <catch2/catch_test_macros.hpp>

#include <string>

#include <nlohmann/json.hpp>

#include "client/http_client.hpp"
#include "core/exit_codes.hpp"
#include "support/api_fixture.hpp"

using json = nlohmann::json;

namespace {

// Me envelope per snapshot v2.1: actor (bio/avatar_url always present,
// empty string = unset) + email (human only) + session (phase-6, absent).
const json kMe = json{
    {"actor",
     json{{"id", "usr_1"},
          {"kind", "human"},
          {"display_name", "Alice"},
          {"bio", "building astral"},
          {"avatar_url", ""}}},
    {"email", "alice@example.com"},
};

} // namespace

TEST_CASE("profile show passes the Me envelope through verbatim with --json") {
    astral_test::ApiFixture fx;
    fx.fake().route("/auth/me", 200, kMe);

    const astral_test::RunResult result = astral_test::runApp({"astral", "profile", "show", "--json"});
    REQUIRE(result.exitCode == 0);

    const json payload = json::parse(result.out);
    REQUIRE(payload == kMe);

    REQUIRE(fx.fake().requests.size() == 2); // well-known + /auth/me
    const astral::client::HttpRequest& request = fx.fake().requests.back();
    REQUIRE(request.method == "GET");
    REQUIRE(request.url.find("/api/v1/auth/me") != std::string::npos);
    REQUIRE(astral_test::requestHasBearer(request, "astral_testtoken"));
}

TEST_CASE("profile show renders display name, email, bio and skips unset avatar") {
    astral_test::ApiFixture fx;
    fx.fake().route("/auth/me", 200, kMe);

    const astral_test::RunResult result = astral_test::runApp({"astral", "profile", "show"});
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.out.find("Alice (usr_1, human) on srv_test (https://api.test)") !=
            std::string::npos);
    REQUIRE(result.out.find("alice@example.com") != std::string::npos);
    REQUIRE(result.out.find("building astral") != std::string::npos);
    // avatar_url is "" (unset): the label must not appear.
    REQUIRE(result.out.find("avatar:") == std::string::npos);
}

TEST_CASE("profile set sends only the provided fields and renders the update") {
    astral_test::ApiFixture fx;
    fx.fake().route("/auth/me", 200, kMe);

    const astral_test::RunResult result = astral_test::runApp(
        {"astral", "profile", "set", "--bio", "hello", "--avatar-url", "https://cdn.test/a.png"});
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.out.find("Profile updated.") != std::string::npos);
    REQUIRE(result.out.find("building astral") != std::string::npos);

    const astral::client::HttpRequest& request = fx.fake().requests.back();
    REQUIRE(request.method == "PATCH");
    const json body = json::parse(request.body);
    REQUIRE(body == json{{"bio", "hello"}, {"avatar_url", "https://cdn.test/a.png"}});
}

TEST_CASE("profile set clears a field with an empty string") {
    astral_test::ApiFixture fx;
    fx.fake().route("/auth/me", 200, kMe);

    const astral_test::RunResult result =
        astral_test::runApp({"astral", "profile", "set", "--bio", ""});
    REQUIRE(result.exitCode == 0);

    const json body = json::parse(fx.fake().requests.back().body);
    REQUIRE(body == json{{"bio", ""}});
}

TEST_CASE("profile set without fields is a usage error before any request") {
    astral_test::ApiFixture fx;

    const astral_test::RunResult result = astral_test::runApp({"astral", "profile", "set"});
    REQUIRE(result.exitCode == static_cast<int>(astral::core::ExitCode::Usage));
    REQUIRE(result.err.find("no profile field given") != std::string::npos);
    REQUIRE(fx.fake().requests.empty()); // usage gate runs before discovery
}

TEST_CASE("profile set rejects an empty display name before any request") {
    astral_test::ApiFixture fx;

    const astral_test::RunResult result =
        astral_test::runApp({"astral", "profile", "set", "--display-name", ""});
    REQUIRE(result.exitCode == static_cast<int>(astral::core::ExitCode::Usage));
    REQUIRE(fx.fake().requests.empty());
}

TEST_CASE("profile set surfaces the server validation envelope") {
    astral_test::ApiFixture fx;
    fx.fake().route("/auth/me", 400,
                    json::parse(astral_test::errorEnvelopeBody(
                        "VALIDATION_FAILED", "display_name must not be blank", false, "req_42")));

    const astral_test::RunResult result =
        astral_test::runApp({"astral", "profile", "set", "--display-name", "   ", "--json"});
    // Other 4xx map to the protocol exit code (ARCHITECTURE.md section 12).
    REQUIRE(result.exitCode == static_cast<int>(astral::core::ExitCode::Protocol));

    const json payload = json::parse(result.out);
    REQUIRE(payload.at("error").at("code") == "VALIDATION_FAILED");
    REQUIRE(payload.at("error").at("request_id") == "req_42");
}

TEST_CASE("profile without any credential is AUTH_REQUIRED") {
    astral_test::ApiFixture fx;
    astral_test::unsetEnv("ASTRAL_TOKEN"); // fixture restores it in its destructor

    const astral_test::RunResult result =
        astral_test::runApp({"astral", "profile", "show", "--json"});
    REQUIRE(result.exitCode == static_cast<int>(astral::core::ExitCode::Auth));
    REQUIRE(json::parse(result.out).at("error").at("code") == "AUTH_REQUIRED");
}

TEST_CASE("whoami reports the human email from the Me envelope") {
    astral_test::ApiFixture fx;
    fx.installSession(); // drops ASTRAL_TOKEN, installs a refreshable session
    fx.fake().route("/auth/me", 200, kMe, 2); // human + machine runs each consume one

    const astral_test::RunResult human =
        astral_test::runApp({"astral", "whoami", "https://api.test"});
    REQUIRE(human.exitCode == 0);
    REQUIRE(human.out.find("alice@example.com") != std::string::npos);

    const astral_test::RunResult machine =
        astral_test::runApp({"astral", "whoami", "https://api.test", "--json"});
    REQUIRE(machine.exitCode == 0);
    const json payload = json::parse(machine.out);
    REQUIRE(payload.at("email") == "alice@example.com");
    REQUIRE(payload.at("actor").at("display_name") == "Alice");
}

TEST_CASE("explicit --server flag reaches identity commands despite absent positional") {
    astral_test::ApiFixture fx;
    fx.fake().route("/auth/me", 200, kMe, 2);

    // Regression (round 29): an engaged-but-empty positional used to shadow
    // the explicit --server flag, so `whoami --server X` failed with
    // "no server given". profile shares the resolver.
    const astral_test::RunResult whoami =
        astral_test::runApp({"astral", "whoami", "--server", "https://api.test", "--json"});
    REQUIRE(whoami.exitCode == static_cast<int>(astral::core::ExitCode::Auth)); // no session
    REQUIRE(json::parse(whoami.out).at("error").at("code") == "AUTH_REQUIRED");

    const astral_test::RunResult profile =
        astral_test::runApp({"astral", "profile", "show", "--server", "https://api.test", "--json"});
    REQUIRE(profile.exitCode == 0);
    REQUIRE(json::parse(profile.out) == kMe);
}
