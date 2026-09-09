// device_flow 状态机的纯逻辑单测：HTTP 传输与等待都用可注入的 fake，
// 不触网、不真等。覆盖 pending 轮询、SLOW_DOWN 退避、拒绝/超时、协议门。
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "auth/device_flow.hpp"
#include "core/error.hpp"

namespace {

using astral::auth::runDeviceFlow;

struct ScriptedResponse {
    int status = 0;
    std::string body;
};

// Minimal scripted transport: request.URL -> (status, body), consumed in
// order for poll calls; unmatched URLs fail the test loudly.
struct FakeHttp {
    std::string wellKnownBody;
    std::string createBody;
    std::vector<ScriptedResponse> polls;
    std::vector<std::string> requestedUrls;

    astral::client::HttpResponse operator()(const astral::client::HttpRequest& request) {
        requestedUrls.push_back(request.url);
        astral::client::HttpResponse response;
        if (request.url.find("/.well-known/astral") != std::string::npos) {
            response.status = 200;
            response.body = wellKnownBody;
            return response;
        }
        if (request.url.find("/auth/device/authorizations/") == std::string::npos) {
            response.status = 201;
            response.body = createBody;
            return response;
        }
        REQUIRE(!polls.empty());
        const ScriptedResponse next = polls.front();
        polls.erase(polls.begin());
        response.status = next.status;
        response.body = next.body;
        return response;
    }
};

std::string errorEnvelope(const std::string& code) {
    return R"({"error":{"code":")" + code + R"(","message":"x","retryable":true}})";
}

const std::string kWellKnown = R"({
  "server_id": "srv_01", "canonical_url": "https://s.example.com",
  "api_base": "/api/v1", "protocol_version": 1, "min_cli_protocol_version": 1
})";

const std::string kAuthorization = R"({
  "device_code": "dev_abc", "user_code": "ABCD-EFGH",
  "verification_uri": "https://s.example.com/device",
  "verification_uri_complete": "https://s.example.com/device?code=ABCD-EFGH",
  "expires_in": 600, "interval": 3
})";

const std::string kTokenPair = R"({"access_token":"at_1","token_type":"Bearer","expires_in":900,)"
                               R"("refresh_token":"rt_1","actor_id":"usr_01"})";

// Wraps the stateful fake so the std::function references the local object
// instead of copying it (recordings must land in `fake`, not in the closure).
template <typename Fake>
astral::auth::HttpFn refHttp(Fake& fake) {
    return [&fake](const astral::client::HttpRequest& request) { return fake(request); };
}

void noSleep(std::chrono::milliseconds) {}

} // namespace

TEST_CASE("device flow completes after a pending poll") {
    FakeHttp fake{kWellKnown,
                  kAuthorization,
                  {{400, errorEnvelope("AUTHORIZATION_PENDING")}, {200, kTokenPair}}};
    std::vector<std::chrono::milliseconds> sleeps;
    std::optional<std::string> shownUrl;
    std::optional<std::string> shownCode;

    const astral::platform::LoginSession session = runDeviceFlow(
        "https://s.example.com", refHttp(fake),
        [&](std::chrono::milliseconds d) { sleeps.push_back(d); },
        [&](const std::string& url, const std::string& code) {
            shownUrl = url;
            shownCode = code;
        });

    CHECK(session.serverUrl == "https://s.example.com");
    CHECK(session.serverId == "srv_01");
    CHECK(session.apiBase == "/api/v1");
    CHECK(session.accessToken == "at_1");
    CHECK(session.refreshToken == "rt_1");
    CHECK(session.principalId == "usr_01");
    REQUIRE(sleeps.size() == 2);
    CHECK(sleeps.front() == std::chrono::seconds(3));
    CHECK(shownUrl == "https://s.example.com/device?code=ABCD-EFGH");
    CHECK(shownCode == "ABCD-EFGH");
    // The poll URL embeds the opaque device code.
    REQUIRE(!fake.requestedUrls.empty());
    CHECK(fake.requestedUrls.back().find("/auth/device/authorizations/dev_abc/token") !=
          std::string::npos);
}

TEST_CASE("slow_down extends the polling interval by five seconds") {
    FakeHttp fake{kWellKnown,
                  kAuthorization,
                  {{400, errorEnvelope("SLOW_DOWN")},
                   {400, errorEnvelope("AUTHORIZATION_PENDING")},
                   {200, kTokenPair}}};
    std::vector<std::chrono::milliseconds> sleeps;

    runDeviceFlow("https://s.example.com", refHttp(fake),
                  [&](std::chrono::milliseconds d) { sleeps.push_back(d); }, {});
    REQUIRE(sleeps.size() == 3);
    CHECK(sleeps[0] == std::chrono::seconds(3));
    CHECK(sleeps[1] == std::chrono::seconds(8));
    CHECK(sleeps[2] == std::chrono::seconds(8));
}

TEST_CASE("denied approval maps to AUTH_REQUIRED") {
    FakeHttp fake{kWellKnown, kAuthorization, {{401, errorEnvelope("TOKEN_REVOKED")}}};
    try {
        runDeviceFlow("https://s.example.com", refHttp(fake), noSleep, {});
        FAIL("expected AstralError");
    } catch (const astral::core::AstralError& error) {
        CHECK(error.code() == astral::core::Errc::AuthRequired);
        CHECK(error.exitCode() == 3);
    }
}

TEST_CASE("device code expiry maps to TIMEOUT") {
    // expires_in=1s: the first 3s wait already passes the deadline.
    FakeHttp fake{kWellKnown,
                  R"({"device_code":"dev_abc","user_code":"ABCD-EFGH",)"
                  R"("verification_uri":"https://s.example.com/device",)"
                  R"("verification_uri_complete":"https://s.example.com/device?c=1",)"
                  R"("expires_in":1,"interval":3})",
                  {}};
    try {
        runDeviceFlow("https://s.example.com", refHttp(fake), noSleep, {});
        FAIL("expected AstralError");
    } catch (const astral::core::AstralError& error) {
        CHECK(error.code() == astral::core::Errc::Timeout);
    }
}

TEST_CASE("protocol version mismatch is rejected before any code exchange") {
    FakeHttp fake{
        R"({"server_id":"srv_01","api_base":"/api/v1","protocol_version":99})", kAuthorization, {}};
    try {
        runDeviceFlow("https://s.example.com", refHttp(fake), noSleep, {});
        FAIL("expected AstralError");
    } catch (const astral::core::AstralError& error) {
        CHECK(error.code() == astral::core::Errc::ProtocolIncompatible);
    }
    // No authorization request was sent.
    CHECK(std::none_of(fake.requestedUrls.begin(), fake.requestedUrls.end(),
                       [](const std::string& url) {
                           return url.find("/auth/device/authorizations") != std::string::npos;
                       }));
}

TEST_CASE("a URL without a scheme is a local input error") {
    FakeHttp fake{kWellKnown, kAuthorization, {}};
    try {
        runDeviceFlow("s.example.com", refHttp(fake), noSleep, {});
        FAIL("expected AstralError");
    } catch (const astral::core::AstralError& error) {
        CHECK(error.code() == astral::core::Errc::LocalWorkspaceError);
    }
    CHECK(fake.requestedUrls.empty());
}
