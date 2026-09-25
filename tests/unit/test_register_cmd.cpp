// astral register 命令的 runApp 级单测（modulator TODO §3 P3 移交项）：
// 邀请码注册 + 默认衔接 device-flow login。HTTP 传输注入脚本化 fake，
// ASTRAL_HOME 指向临时目录隔离凭证文件；register 是匿名端点，ASTRAL_TOKEN
// 必须不被带上。device flow 用 interval=0 脚本，realSleep(0) 为空操作。
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "client/http_client.hpp"
#include "core/exit_codes.hpp"
#include "platform/credential_store.hpp"
#include "support/api_fixture.hpp"

namespace {

using json = nlohmann::json;
using astral_test::ApiFixture;
using astral_test::runApp;
using astral_test::RunResult;

// Me envelope per snapshot v2.1（register 201 响应 = Me + Set-Cookie）。
const json kMe = json{{"actor", json{{"id", "usr_1"},
                                     {"kind", "human"},
                                     {"display_name", "Alice"},
                                     {"bio", ""},
                                     {"avatar_url", ""}}},
                      {"email", "alice@example.com"},
                      {"platform_role", "admin"}};

json errorBody(const std::string& code, const std::string& message, bool retryable = false) {
    return json{{"error",
                 {{"code", code},
                  {"message", message},
                  {"retryable", retryable},
                  {"request_id", "req_1"}}}};
}

std::vector<std::string> registerArgs() {
    return {"astral",
            "register",
            "https://api.test",
            "--email",
            "alice@example.com",
            "--password",
            "passw0rd1",
            "--display-name",
            "Alice",
            "--invite-code",
            "ABCDE-FGHIJ-KLMNO-PQRST"};
}

// /token 路由必须先于 create 路由登记：FakeApi 按子串首中即消费，轮询 URL
// 也包含 "/auth/device/authorizations"。
void routeDeviceFlowGrant(astral_test::FakeApi& fake) {
    fake.route(
        "/token", 200,
        json{{"access_token", "at_new"}, {"refresh_token", "rt_new"}, {"actor_id", "usr_1"}});
    fake.route("/auth/device/authorizations", 201,
               json{{"device_code", "dev_1"},
                    {"user_code", "ABCD-EFGH"},
                    {"verification_uri_complete", "https://api.test/device?user_code=ABCD-EFGH"},
                    {"interval", 0},
                    {"expires_in", 600}});
}

bool hasHeader(const astral::client::HttpRequest& request, const std::string& name) {
    for (const auto& [key, value] : request.headers) {
        if (key == name) {
            return true;
        }
    }
    return false;
}

TEST_CASE("register posts the anonymous invite body and chains device-flow login") {
    ApiFixture fx;
    fx.fake().route("/auth/register", 201, kMe);
    routeDeviceFlowGrant(fx.fake());

    const RunResult result = runApp(registerArgs());
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.out.find("Registered Alice (usr_1) on https://api.test") != std::string::npos);
    REQUIRE(result.out.find("email:  alice@example.com") != std::string::npos);
    REQUIRE(result.out.find("Logged in as usr_1 on srv_test (https://api.test)") !=
            std::string::npos);
    // device-flow 提示走 stderr（login 同款约定），stdout 保持给人读的正文。
    REQUIRE(result.err.find("approve at: https://api.test/device") != std::string::npos);

    // well-known(register) + register + well-known(device flow) + create + poll
    REQUIRE(fx.fake().requests.size() == 5);
    const astral::client::HttpRequest& registerRequest = fx.fake().requests[1];
    REQUIRE(registerRequest.method == "POST");
    REQUIRE(registerRequest.url == "https://api.test/api/v1/auth/register");
    // 匿名端点：即便环境里有 ASTRAL_TOKEN 也不得携带。
    REQUIRE_FALSE(registerRequest.bearerToken.has_value());
    REQUIRE(hasHeader(registerRequest, "X-Astral-Client"));
    const json body = json::parse(registerRequest.body);
    REQUIRE(body == json{{"email", "alice@example.com"},
                         {"password", "passw0rd1"},
                         {"display_name", "Alice"},
                         {"invite_code", "ABCDE-FGHIJ-KLMNO-PQRST"}});

    // 登录半程的 token 对已落盘（凭证槽 = server URL）。
    auto store = astral::platform::makeDefaultCredentialStore();
    const auto session = store->loadSession("https://api.test");
    REQUIRE(session.has_value());
    REQUIRE(session->accessToken == "at_new");
    REQUIRE(session->refreshToken == "rt_new");
    REQUIRE(session->principalId == "usr_1");
}

TEST_CASE("register --json outputs the registered identity and the login result") {
    ApiFixture fx;
    fx.fake().route("/auth/register", 201, kMe);
    routeDeviceFlowGrant(fx.fake());

    std::vector<std::string> args = registerArgs();
    args.push_back("--json");
    const RunResult result = runApp(args);
    REQUIRE(result.exitCode == 0);

    const json payload = json::parse(result.out);
    REQUIRE(payload["registered"] == kMe);
    REQUIRE(payload["logged_in"] == true);
    REQUIRE(payload["session"] == json{{"server_url", "https://api.test"},
                                       {"server_id", "srv_test"},
                                       {"principal_id", "usr_1"}});
}

TEST_CASE("register --no-login creates the account and stops there") {
    ApiFixture fx;
    fx.fake().route("/auth/register", 201, kMe);

    std::vector<std::string> args = registerArgs();
    args.push_back("--no-login");
    args.push_back("--json");
    const RunResult result = runApp(args);
    REQUIRE(result.exitCode == 0);

    // 只剩 discovery + register 两个请求：没有 device flow。
    REQUIRE(fx.fake().requests.size() == 2);
    const json payload = json::parse(result.out);
    REQUIRE(payload["registered"] == kMe);
    REQUIRE(payload["logged_in"] == false);
    REQUIRE_FALSE(payload.contains("session"));
}

TEST_CASE("register --bootstrap omits invite_code from the body") {
    ApiFixture fx;
    fx.fake().route("/auth/register", 201, kMe);

    const RunResult result =
        runApp({"astral", "register", "https://api.test", "--email", "alice@example.com",
                "--password", "passw0rd1", "--bootstrap", "--no-login"});
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.out.find("Registered Alice (usr_1) on https://api.test") != std::string::npos);

    const astral::client::HttpRequest& registerRequest = fx.fake().requests.back();
    const json body = json::parse(registerRequest.body);
    REQUIRE(body == json{{"email", "alice@example.com"}, {"password", "passw0rd1"}});
    REQUIRE_FALSE(body.contains("invite_code"));
}

TEST_CASE("register without --invite-code or --bootstrap is a usage error") {
    ApiFixture fx;
    fx.fake().route("/auth/register", 201, kMe);

    std::vector<std::string> args = registerArgs();
    args.pop_back(); // 丢掉 invite code 值
    args.pop_back(); // 丢掉 --invite-code
    const RunResult result = runApp(args);
    REQUIRE(result.exitCode == static_cast<int>(astral::core::ExitCode::Usage));
    REQUIRE(result.err.find("invite") != std::string::npos);
    // 用法错误在 discovery 之前拦下，不发出任何请求。
    REQUIRE(fx.fake().requests.empty());
}

TEST_CASE("register --invite-code together with --bootstrap is a usage error") {
    ApiFixture fx;
    const RunResult result = runApp({"astral", "register", "https://api.test", "--email",
                                     "alice@example.com", "--password", "passw0rd1",
                                     "--invite-code", "ABCDE-FGHIJ-KLMNO-PQRST", "--bootstrap"});
    REQUIRE(result.exitCode == static_cast<int>(astral::core::ExitCode::Usage));
    REQUIRE(fx.fake().requests.empty());
}

TEST_CASE("register INVITE_INVALID is exit 1 with the server detail in the message") {
    ApiFixture fx;
    fx.fake().route("/auth/register", 400, errorBody("INVITE_INVALID", "invite code is invalid"));

    const RunResult result = runApp(registerArgs());
    REQUIRE(result.exitCode == static_cast<int>(astral::core::ExitCode::GenericFailure));
    REQUIRE(result.err.find("[REGISTRATION_REJECTED]") != std::string::npos);
    REQUIRE(result.err.find("invite code rejected") != std::string::npos);
    REQUIRE(result.err.find("invite code is invalid") != std::string::npos);
}

TEST_CASE("register rejections surface the protocol code in --json") {
    ApiFixture fx;
    fx.fake().route("/auth/register", 400, errorBody("INVITE_INVALID", "invite code is invalid"));

    std::vector<std::string> args = registerArgs();
    args.push_back("--json");
    const RunResult result = runApp(args);
    REQUIRE(result.exitCode == static_cast<int>(astral::core::ExitCode::GenericFailure));

    const json payload = json::parse(result.out);
    REQUIRE(payload["error"]["code"] == "INVITE_INVALID");
    REQUIRE(payload["error"]["message"].get<std::string>().find("invite code rejected") !=
            std::string::npos);
    REQUIRE(payload["error"]["request_id"] == "req_1");
}

TEST_CASE("register EMAIL_TAKEN is exit 1 and notes the invite was not consumed") {
    ApiFixture fx;
    fx.fake().route("/auth/register", 409, errorBody("EMAIL_TAKEN", "email in use"));

    const RunResult result = runApp(registerArgs());
    REQUIRE(result.exitCode == static_cast<int>(astral::core::ExitCode::GenericFailure));
    REQUIRE(result.err.find("already registered") != std::string::npos);
    REQUIRE(result.err.find("not consumed") != std::string::npos);
}

TEST_CASE("register bootstrap branch on a populated server is exit 1 with invite hint") {
    ApiFixture fx;
    fx.fake().route(
        "/auth/register", 403,
        errorBody("INSUFFICIENT_SCOPE", "registration closed: initial human already exists"));

    const RunResult result =
        runApp({"astral", "register", "https://api.test", "--email", "alice@example.com",
                "--password", "passw0rd1", "--bootstrap"});
    REQUIRE(result.exitCode == static_cast<int>(astral::core::ExitCode::GenericFailure));
    REQUIRE(result.err.find("an invite code is required") != std::string::npos);
    REQUIRE(result.err.find("registration closed") != std::string::npos);
}

TEST_CASE("register 429 is exit 1 and surfaces Retry-After seconds") {
    ApiFixture fx;
    fx.fake().routeWithHeaders("/auth/register", 429,
                               errorBody("RATE_LIMITED", "too many registrations", true),
                               {{"Retry-After", "42"}});

    const RunResult result = runApp(registerArgs());
    REQUIRE(result.exitCode == static_cast<int>(astral::core::ExitCode::GenericFailure));
    REQUIRE(result.err.find("retry after 42s") != std::string::npos);
}

TEST_CASE("register contract-external 5xx keeps the shared status mapping") {
    ApiFixture fx;
    fx.fake().route("/auth/register", 500, errorBody("INTERNAL", "boom", true));

    const RunResult result = runApp(registerArgs());
    REQUIRE(result.exitCode == static_cast<int>(astral::core::ExitCode::Network));
    REQUIRE(result.err.find("register") != std::string::npos);
}

TEST_CASE("register keeps the failure code when the automatic login is denied") {
    ApiFixture fx;
    fx.fake().route("/auth/register", 201, kMe);
    fx.fake().route("/token", 401, errorBody("ACCESS_DENIED", "denied"));
    fx.fake().route(
        "/auth/device/authorizations", 201,
        json{{"device_code", "dev_1"},
             {"user_code", "ABCD-EFGH"},
             {"verification_uri_complete", "https://api.test/device?user_code=ABCD-EFGH"},
             {"interval", 0},
             {"expires_in", 600}});

    const RunResult result = runApp(registerArgs());
    // 登录半程按失败本身退出（device flow 拒绝 = 3），不吞不降级。
    REQUIRE(result.exitCode == static_cast<int>(astral::core::ExitCode::Auth));
    REQUIRE(result.err.find("account created") != std::string::npos);
    REQUIRE(result.err.find("astral login https://api.test") != std::string::npos);
}

} // namespace
