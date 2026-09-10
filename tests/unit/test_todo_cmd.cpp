// astral todo 命令族的 runApp 级单测：HTTP 传输注入脚本化 fake，ASTRAL_HOME
// 与工作目录指向临时目录（隔离凭证文件与 .astral 绑定），认证走 ASTRAL_TOKEN
// （或临时会话文件覆盖惰性刷新路径）。不触网。
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "app/app.hpp"
#include "auth/session.hpp"
#include "client/http_client.hpp"
#include "core/exit_codes.hpp"
#include "platform/credential_store.hpp"
#include "support/api_fixture.hpp"
#include "workspace/binding.hpp"

namespace {

namespace fs = std::filesystem;
namespace client = astral::client;
namespace platform = astral::platform;
namespace workspace = astral::workspace;
using json = nlohmann::json;
using astral_test::ApiFixture;
using astral_test::CwdGuard;
using astral_test::EnvGuard;
using astral_test::errorEnvelopeBody;
using astral_test::requestHasBearer;
using astral_test::runApp;
using astral_test::RunResult;
using astral_test::uniqueHome;

const std::string kWellKnown = astral_test::ApiFixture::wellKnown().dump();

const json kTaskOne = json{
    {"id", "task_1"},
    {"workspace_id", "ws_1"},
    {"parent_id", nullptr},
    {"title", "Fix login flow"},
    {"description", "steps inside"},
    {"status", "open"},
    {"priority", "high"},
    {"assignee_actor_id", nullptr},
    {"revision", 3},
    {"lease", nullptr},
    {"created_at", "2026-09-10T00:00:00Z"},
    {"updated_at", "2026-09-10T00:00:00Z"},
};

const json kTaskPage = json{{"items", json::array({kTaskOne})}, {"next_cursor", nullptr}};

const json kClaimResult =
    json{{"task", kTaskOne},
         {"lease", {{"holder_actor_id", "usr_1"}, {"expires_at", "2026-09-10T01:00:00Z"}}}};

// ---- tests ---------------------------------------------------------------

TEST_CASE("todo list --json prints the page plus workspace context") {
    ApiFixture fx;
    fx.fake().route("/workspaces/ws_1/children", 200, kTaskPage);

    const RunResult result = runApp({"astral", "todo", "list", "--json"});
    REQUIRE(result.exitCode == 0);

    const json payload = json::parse(result.out);
    REQUIRE(payload.at("workspace_id") == "ws_1");
    REQUIRE(payload.at("items").size() == 1);
    REQUIRE(payload.at("items").at(0).at("id") == "task_1");
    REQUIRE(payload.at("next_cursor").is_null());

    REQUIRE(fx.fake().requests.size() == 2); // well-known + list
    REQUIRE(fx.fake().requests[0].url.find("/.well-known/astral") != std::string::npos);
    REQUIRE(fx.fake().requests[1].url.find("/workspaces/ws_1/children") != std::string::npos);
    // ASTRAL_TOKEN path: bearer is the env credential.
    REQUIRE(requestHasBearer(fx.fake().requests[1], "astral_testtoken"));
}

TEST_CASE("todo list --all follows the server cursor and merges pages") {
    ApiFixture fx;
    fx.fake().route("/workspaces/ws_1/children", 200,
                    json{{"items", json::array({kTaskOne})}, {"next_cursor", "50"}});
    fx.fake().route("/workspaces/ws_1/children", 200,
                    json{{"items", json::array({kTaskOne})}, {"next_cursor", nullptr}});

    const RunResult result = runApp({"astral", "todo", "list", "--all", "--json"});
    REQUIRE(result.exitCode == 0);

    const json payload = json::parse(result.out);
    REQUIRE(payload.at("items").size() == 2);
    REQUIRE(payload.at("next_cursor").is_null());
    REQUIRE(fx.fake().requests.size() == 3); // well-known + two pages
    bool sawCursorRequest = false;
    for (const auto& request : fx.fake().requests) {
        if (request.url.find("cursor=50") != std::string::npos) {
            sawCursorRequest = true;
        }
    }
    REQUIRE(sawCursorRequest);
}

TEST_CASE("todo list human output carries id, status and title without escapes") {
    ApiFixture fx;
    fx.fake().route("/workspaces/ws_1/children", 200, kTaskPage);

    const RunResult result = runApp({"astral", "todo", "list"});
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.out.find("ID") == 0);
    REQUIRE(result.out.find("Fix login flow") != std::string::npos);
    REQUIRE(result.out.find('\x1b') == std::string::npos);
}

TEST_CASE("todo show --json returns the task verbatim including tags") {
    ApiFixture fx;
    json task = kTaskOne;
    task["tags"] = json::array({json{{"id", "tag_1"}, {"workspace_id", "ws_1"}, {"name", "auth"}}});
    fx.fake().route("/tasks/task_1", 200, task);

    const RunResult result = runApp({"astral", "todo", "show", "task_1", "--json"});
    REQUIRE(result.exitCode == 0);

    const json payload = json::parse(result.out);
    REQUIRE(payload == task);
    REQUIRE(fx.fake().requests.back().url.find("/tasks/task_1") != std::string::npos);
}

TEST_CASE("todo add posts the create body and prints the created task") {
    ApiFixture fx;
    fx.fake().route("/workspaces/ws_1/children", 201, kTaskOne);

    const RunResult result =
        runApp({"astral", "todo", "add", "Fix login flow", "--priority", "high", "--json"});
    REQUIRE(result.exitCode == 0);

    const json payload = json::parse(result.out);
    REQUIRE(payload.at("id") == "task_1");

    const client::HttpRequest& request = fx.fake().requests.back();
    REQUIRE(request.method == "POST");
    const json body = json::parse(request.body);
    REQUIRE(body.at("title") == "Fix login flow");
    REQUIRE(body.at("priority") == "high");
}

TEST_CASE("todo claim reads the current revision then posts it") {
    ApiFixture fx;
    // Ordered consumption: first /tasks/task_1 hit is the revision GET, the
    // /claim hit is the POST (its URL also contains the shorter needles, so
    // routes must be declared with the claim route consuming its own hit).
    fx.fake().route("/tasks/task_1/claim", 200, kClaimResult);
    fx.fake().route("/tasks/task_1", 200, kTaskOne);

    const RunResult result = runApp({"astral", "todo", "claim", "task_1", "--json"});
    REQUIRE(result.exitCode == 0);

    const json payload = json::parse(result.out);
    REQUIRE(payload.at("task").at("id") == "task_1");
    REQUIRE(payload.at("lease").at("holder_actor_id") == "usr_1");

    // GET (revision read) precedes the claim POST; lease default applied.
    const client::HttpRequest& claimRequest = fx.fake().requests.back();
    REQUIRE(claimRequest.method == "POST");
    const json body = json::parse(claimRequest.body);
    REQUIRE(body.at("expected_revision") == 3);
    REQUIRE(body.at("lease_seconds") == 300);
}

TEST_CASE("todo claim --revision skips the lookup and pins the body") {
    ApiFixture fx;
    fx.fake().route("/tasks/task_1/claim", 200, kClaimResult);

    const RunResult result =
        runApp({"astral", "todo", "claim", "task_1", "--revision", "7", "--json"});
    REQUIRE(result.exitCode == 0);
    REQUIRE(fx.fake().requests.size() == 2); // well-known + claim only

    const json body = json::parse(fx.fake().requests.back().body);
    REQUIRE(body.at("expected_revision") == 7);
}

TEST_CASE("todo done patches status with optimistic concurrency") {
    ApiFixture fx;
    json doneTask = kTaskOne;
    doneTask["status"] = "done";
    doneTask["revision"] = 4;
    fx.fake().route("/tasks/task_1", 200, kTaskOne); // revision read
    fx.fake().route("/tasks/task_1", 200, doneTask); // PATCH reply

    const RunResult result = runApp({"astral", "todo", "done", "task_1", "--json"});
    REQUIRE(result.exitCode == 0);

    const json payload = json::parse(result.out);
    REQUIRE(payload.at("status") == "done");

    const client::HttpRequest& patch = fx.fake().requests.back();
    REQUIRE(patch.method == "PATCH");
    const json body = json::parse(patch.body);
    REQUIRE(body.at("expected_revision") == 3);
    REQUIRE(body.at("status") == "done");
}

TEST_CASE("server conflict surfaces as exit 5 with the protocol code") {
    ApiFixture fx;
    fx.fake().route("/tasks/task_1/claim", 409,
                    json::parse(errorEnvelopeBody("TASK_ALREADY_CLAIMED", "someone holds the lease",
                                                  false, "req_conflict")));
    fx.fake().route("/tasks/task_1", 200, kTaskOne);

    const RunResult result = runApp({"astral", "todo", "claim", "task_1", "--json"});
    REQUIRE(result.exitCode == static_cast<int>(astral::core::ExitCode::Conflict));

    const json payload = json::parse(result.out);
    REQUIRE(payload.at("error").at("code") == "TASK_ALREADY_CLAIMED");
    REQUIRE(payload.at("error").at("request_id") == "req_conflict");
    REQUIRE(payload.at("error").at("retryable") == false);
}

TEST_CASE("server 404 surfaces as exit 4 with the protocol code") {
    ApiFixture fx;
    fx.fake().route("/tasks/task_missing", 404,
                    json::parse(errorEnvelopeBody("TASK_NOT_FOUND", "no such task")));

    const RunResult result = runApp({"astral", "todo", "show", "task_missing", "--json"});
    REQUIRE(result.exitCode == static_cast<int>(astral::core::ExitCode::NotFound));

    const json payload = json::parse(result.out);
    REQUIRE(payload.at("error").at("code") == "TASK_NOT_FOUND");
}

TEST_CASE("stale env token 401 maps to auth failure with protocol code") {
    ApiFixture fx;
    fx.fake().route("/workspaces/ws_1/children", 401,
                    json::parse(errorEnvelopeBody("TOKEN_EXPIRED", "access token expired", true)));

    const RunResult result = runApp({"astral", "todo", "list", "--json"});
    REQUIRE(result.exitCode == static_cast<int>(astral::core::ExitCode::Auth));

    const json payload = json::parse(result.out);
    REQUIRE(payload.at("error").at("code") == "TOKEN_EXPIRED");
    REQUIRE(payload.at("error").at("retryable") == true);
}

TEST_CASE("expired human session refreshes once and replays the request") {
    ApiFixture fx;
    fx.installSession();
    fx.fake().route("/auth/token/refresh", 200,
                    json{{"access_token", "at_new"},
                         {"refresh_token", "rt_new"},
                         {"expires_in", 900},
                         {"actor_id", "usr_1"}});
    // First list attempt 401s; the post-refresh replay is served by the
    // second, later-declared route.
    fx.fake().route("/workspaces/ws_1/children", 401,
                    json::parse(errorEnvelopeBody("TOKEN_EXPIRED", "access token expired")));
    fx.fake().route("/workspaces/ws_1/children", 200, kTaskPage);

    const RunResult result = runApp({"astral", "todo", "list", "--json"});
    REQUIRE(result.exitCode == 0);

    // Request order: well-known, 401, refresh, replay.
    REQUIRE(fx.fake().requests.size() == 4);
    REQUIRE(fx.fake().requests[2].url.find("/auth/token/refresh") != std::string::npos);
    REQUIRE(json::parse(fx.fake().requests[2].body).at("refresh_token") == "rt_old");
    REQUIRE(requestHasBearer(fx.fake().requests[3], "at_new"));

    // The rotated pair is persisted for subsequent commands.
    auto store = platform::makeDefaultCredentialStore();
    const auto session = store->loadSession("https://api.test");
    REQUIRE(session.has_value());
    REQUIRE(session->accessToken == "at_new");
    REQUIRE(session->refreshToken == "rt_new");

    const json payload = json::parse(result.out);
    REQUIRE(payload.at("items").size() == 1);
}

TEST_CASE("revoked refresh family clears the session and reports auth failure") {
    ApiFixture fx;
    fx.installSession();
    fx.fake().route("/auth/token/refresh", 401,
                    json::parse(errorEnvelopeBody("TOKEN_REVOKED", "family revoked")));
    // Triggers the refresh path with an initial 401.
    fx.fake().route("/workspaces/ws_1/children", 401,
                    json::parse(errorEnvelopeBody("TOKEN_EXPIRED", "access token expired")));

    const RunResult result = runApp({"astral", "todo", "list", "--json"});
    REQUIRE(result.exitCode == static_cast<int>(astral::core::ExitCode::Auth));

    auto store = platform::makeDefaultCredentialStore();
    REQUIRE_FALSE(store->loadSession("https://api.test").has_value());
}

TEST_CASE("todo search with zero filters is a usage error") {
    ApiFixture fx;
    const RunResult result = runApp({"astral", "todo", "search", "--json"});
    REQUIRE(result.exitCode == static_cast<int>(astral::core::ExitCode::Usage));
    const json payload = json::parse(result.out);
    REQUIRE(payload.at("error").at("code") == "USAGE");
}

TEST_CASE("todo search with a structured filter alone is valid in v2") {
    ApiFixture fx;
    fx.fake().route("/task-search", 200, json{{"items", json::array()}, {"next_cursor", nullptr}});

    const RunResult result = runApp({"astral", "todo", "search", "--status", "open", "--json"});
    REQUIRE(result.exitCode == 0);
    std::string searchUrl;
    for (const auto& request : fx.fake().requests) {
        if (request.url.find("/task-search") != std::string::npos) {
            searchUrl = request.url;
        }
    }
    REQUIRE(searchUrl.find("status=open") != std::string::npos);
}

TEST_CASE("todo search encodes free-form query parameters") {
    ApiFixture fx;
    fx.fake().route("/task-search", 200, json{{"items", json::array()}, {"next_cursor", nullptr}});

    const RunResult result = runApp({"astral", "todo", "search", "--regex", "(login|auth)$",
                                     "--fuzzy", "log in", "--tag", "auth core", "--json"});
    REQUIRE(result.exitCode == 0);

    std::string searchUrl;
    for (const auto& request : fx.fake().requests) {
        if (request.url.find("/task-search") != std::string::npos) {
            searchUrl = request.url;
        }
    }
    REQUIRE_FALSE(searchUrl.empty());
    // Escaped regex metacharacters and spaces must be percent-encoded.
    REQUIRE(searchUrl.find("regex=%28login%7Cauth%29%24") != std::string::npos);
    REQUIRE(searchUrl.find("fuzzy=log%20in") != std::string::npos);
    REQUIRE(searchUrl.find("tag=auth%20core") != std::string::npos);
}

TEST_CASE("todo list --parent switches to the task container collection") {
    ApiFixture fx;
    fx.fake().route("/tasks/task_1/children", 200, kTaskPage);

    const RunResult result = runApp({"astral", "todo", "list", "--parent", "task_1", "--json"});
    REQUIRE(result.exitCode == 0);

    const json payload = json::parse(result.out);
    REQUIRE(payload.at("items").size() == 1);
    bool sawContainerPath = false;
    for (const auto& request : fx.fake().requests) {
        if (request.url.find("/tasks/task_1/children") != std::string::npos) {
            sawContainerPath = true;
        }
        REQUIRE(request.url.find("/workspaces/ws_1/children") == std::string::npos);
    }
    REQUIRE(sawContainerPath);
}

TEST_CASE("todo add --parent posts into the task container without a parent_id body") {
    ApiFixture fx;
    fx.fake().route("/tasks/task_9/children", 201, kTaskOne);

    const RunResult result =
        runApp({"astral", "todo", "add", "Write DDL", "--parent", "task_9", "--json"});
    REQUIRE(result.exitCode == 0);

    const client::HttpRequest& request = fx.fake().requests.back();
    REQUIRE(request.method == "POST");
    REQUIRE(request.url.find("/tasks/task_9/children") != std::string::npos);
    const json body = json::parse(request.body);
    REQUIRE(body.at("title") == "Write DDL");
    REQUIRE_FALSE(body.contains("parent_id"));
}

TEST_CASE("todo without any resolvable target is a local workspace error") {
    const fs::path home = uniqueHome("astral-test-bare");
    fs::create_directories(home / "work");
    EnvGuard homeGuard("ASTRAL_HOME", home.string());
    EnvGuard noToken("ASTRAL_TOKEN", "");
    EnvGuard noServer("ASTRAL_SERVER", "");
    CwdGuard cwd(home / "work");

    astral::auth::setCommandTransportForTests(
        [](const client::HttpRequest&) -> client::HttpResponse {
            FAIL("no network expected without a resolvable target");
            return client::HttpResponse{};
        });

    const RunResult result = runApp({"astral", "todo", "list", "--json"});
    astral::auth::setCommandTransportForTests(nullptr);

    REQUIRE(result.exitCode == static_cast<int>(astral::core::ExitCode::LocalWorkspace));
    const json payload = json::parse(result.out);
    REQUIRE(payload.at("error").at("code") == "LOCAL_WORKSPACE_ERROR");
}

} // namespace
