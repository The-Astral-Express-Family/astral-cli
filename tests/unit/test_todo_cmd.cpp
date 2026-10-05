// astral todo 命令族的 runApp 级单测：HTTP 传输注入脚本化 fake，ASTRAL_HOME
// 与工作目录指向临时目录（隔离凭证文件与 .astral 绑定），认证走 ASTRAL_TOKEN
// （或临时会话文件覆盖惰性刷新路径）。不触网。
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
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
    {"created_at", "2026-09-10T00:00:00Z"},
    {"updated_at", "2026-09-10T00:00:00Z"},
};

const json kTaskPage = json{{"items", json::array({kTaskOne})}, {"next_cursor", nullptr}};

// 2.3：ClaimResult = {task}（租约拆除，认领持有至 release）。
const json kClaimed = json{{"id", "task_1"}, {"assignee_actor_id", "agt_a1"}, {"revision", 4}};

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
    fx.fake().route("/tasks/task_1/claim", 200, kClaimed);
    fx.fake().route("/tasks/task_1", 200, kTaskOne);

    const RunResult result = runApp({"astral", "todo", "claim", "task_1", "--json"});
    REQUIRE(result.exitCode == 0);

    const json payload = json::parse(result.out);
    REQUIRE(payload.at("id") == "task_1");
    REQUIRE(payload.at("assignee_actor_id") == "agt_a1");

    // GET (revision read) precedes the claim POST; no lease_seconds (2.3).
    const client::HttpRequest& claimRequest = fx.fake().requests.back();
    REQUIRE(claimRequest.method == "POST");
    const json body = json::parse(claimRequest.body);
    REQUIRE(body.at("expected_revision") == 3);
    REQUIRE(body.find("lease_seconds") == body.end());
}

TEST_CASE("todo claim --revision skips the lookup and pins the body") {
    ApiFixture fx;
    fx.fake().route("/tasks/task_1/claim", 200, kClaimed);

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
    fx.fake().route(
        "/tasks/task_1/claim", 409,
        json::parse(errorEnvelopeBody("TASK_ALREADY_CLAIMED", "someone already claims this task",
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

TEST_CASE("todo search with --blocked-by alone satisfies the filter rule (2.6.1)") {
    ApiFixture fx;
    fx.fake().route("/task-search", 200, json{{"items", json::array()}, {"next_cursor", nullptr}});

    const RunResult result =
        runApp({"astral", "todo", "search", "--blocked-by", "task_9", "--json"});
    REQUIRE(result.exitCode == 0);
    std::string searchUrl;
    for (const auto& request : fx.fake().requests) {
        if (request.url.find("/task-search") != std::string::npos) {
            searchUrl = request.url;
        }
    }
    REQUIRE(searchUrl.find("blocked_by=task_9") != std::string::npos);
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

TEST_CASE("todo add without any target carries the D14 default-workspace hint") {
    astral_test::EnvGuard home("ASTRAL_HOME", uniqueHome("astral-test-hint").string());
    astral_test::EnvGuard noToken("ASTRAL_TOKEN", "");
    astral_test::EnvGuard noServer("ASTRAL_SERVER", "");
    const auto workDir = uniqueHome("astral-test-hint-work");
    std::filesystem::create_directories(workDir);
    CwdGuard cwd(workDir);

    astral::auth::setCommandTransportForTests(
        [](const client::HttpRequest&) -> client::HttpResponse {
            FAIL("no network expected without a resolvable target");
            return client::HttpResponse{};
        });

    const RunResult result = runApp({"astral", "todo", "add", "thing"});
    astral::auth::setCommandTransportForTests(nullptr);

    REQUIRE(result.exitCode == static_cast<int>(astral::core::ExitCode::LocalWorkspace));
    // Human output (stderr) surfaces the D14 convention hint.
    REQUIRE(result.err.find("default/<your-name>/todo") != std::string::npos);
}

// ---- update / release / tag（协议 v2 扩展面） --------------------------------

const json kTagPage = json{
    {"items", json::array({json{{"id", "tag_9"}, {"workspace_id", "ws_1"}, {"name", "auth"}}})},
    {"next_cursor", nullptr}};

json taskWithTags() {
    json task = kTaskOne;
    task["tags"] = json::array({json{{"id", "tag_9"}, {"workspace_id", "ws_1"}, {"name", "auth"}}});
    return task;
}

TEST_CASE("todo update patches mutable fields with optimistic concurrency") {
    ApiFixture fx;
    json updated = kTaskOne;
    updated["title"] = "New title";
    updated["priority"] = "low";
    updated["assignee_actor_id"] = "usr_2";
    updated["revision"] = 4;
    fx.fake().route("/tasks/task_1", 200, kTaskOne); // revision read
    fx.fake().route("/tasks/task_1", 200, updated);  // PATCH reply

    const RunResult result = runApp({"astral", "todo", "update", "task_1", "--title", "New title",
                                     "--priority", "low", "--assignee", "usr_2", "--json"});
    REQUIRE(result.exitCode == 0);

    const json payload = json::parse(result.out);
    REQUIRE(payload == updated);

    const client::HttpRequest& patch = fx.fake().requests.back();
    REQUIRE(patch.method == "PATCH");
    const json body = json::parse(patch.body);
    REQUIRE(body.at("expected_revision") == 3);
    REQUIRE(body.at("title") == "New title");
    REQUIRE(body.at("priority") == "low");
    REQUIRE(body.at("assignee_actor_id") == "usr_2");
    // Untouched fields stay absent (partial update semantics).
    REQUIRE_FALSE(body.contains("description"));
    REQUIRE_FALSE(body.contains("status"));
}

TEST_CASE("todo update --revision skips the lookup and pins the body") {
    ApiFixture fx;
    json updated = kTaskOne;
    updated["status"] = "blocked";
    updated["revision"] = 10;
    fx.fake().route("/tasks/task_1", 200, updated);

    const RunResult result = runApp(
        {"astral", "todo", "update", "task_1", "--status", "blocked", "--revision", "9", "--json"});
    REQUIRE(result.exitCode == 0);
    REQUIRE(fx.fake().requests.size() == 2); // well-known + PATCH only

    const json body = json::parse(fx.fake().requests.back().body);
    REQUIRE(body.at("expected_revision") == 9);
    REQUIRE(body.at("status") == "blocked");
}

TEST_CASE("todo update --assignee - clears the assignee with null") {
    ApiFixture fx;
    fx.fake().route("/tasks/task_1", 200, kTaskOne);

    const RunResult result = runApp(
        {"astral", "todo", "update", "task_1", "--assignee", "-", "--revision", "9", "--json"});
    REQUIRE(result.exitCode == 0);

    const json body = json::parse(fx.fake().requests.back().body);
    REQUIRE(body.at("assignee_actor_id").is_null());
}

TEST_CASE("todo update without mutable fields is a usage error") {
    ApiFixture fx;
    const RunResult result = runApp({"astral", "todo", "update", "task_1", "--json"});
    REQUIRE(result.exitCode == static_cast<int>(astral::core::ExitCode::Usage));
    const json payload = json::parse(result.out);
    REQUIRE(payload.at("error").at("code") == "USAGE");
}

TEST_CASE("todo update conflict surfaces the REVISION_CONFLICT protocol code") {
    ApiFixture fx;
    fx.fake().route(
        "/tasks/task_1", 409,
        json::parse(errorEnvelopeBody("REVISION_CONFLICT", "stale revision", false, "req_rev")));

    const RunResult result =
        runApp({"astral", "todo", "update", "task_1", "--title", "x", "--revision", "3", "--json"});
    REQUIRE(result.exitCode == static_cast<int>(astral::core::ExitCode::Conflict));

    const json payload = json::parse(result.out);
    REQUIRE(payload.at("error").at("code") == "REVISION_CONFLICT");
    REQUIRE(payload.at("error").at("request_id") == "req_rev");
}

TEST_CASE("todo release deletes the claim and confirms with one object") {
    ApiFixture fx;
    fx.fake().route("/tasks/task_1/claim", 204, json::object());

    const RunResult result = runApp({"astral", "todo", "release", "task_1", "--json"});
    REQUIRE(result.exitCode == 0);

    const json payload = json::parse(result.out);
    REQUIRE(payload.at("released") == true);
    REQUIRE(payload.at("task_id") == "task_1");

    const client::HttpRequest& del = fx.fake().requests.back();
    REQUIRE(del.method == "DELETE");
    REQUIRE(del.url.find("/tasks/task_1/claim") != std::string::npos);
}

TEST_CASE("todo release human output prints one confirmation line") {
    ApiFixture fx;
    fx.fake().route("/tasks/task_1/claim", 204, json::object());

    const RunResult result = runApp({"astral", "todo", "release", "task_1"});
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.out.find("Released claim on task_1") == 0);
}

TEST_CASE("todo tag without a subcommand is a usage error") {
    ApiFixture fx;
    const RunResult result = runApp({"astral", "todo", "tag"});
    REQUIRE(result.exitCode == static_cast<int>(astral::core::ExitCode::Usage));
}

TEST_CASE("todo tag attach resolves the tag name via the workspace dictionary") {
    ApiFixture fx;
    fx.fake().route("/workspaces/ws_1/tags", 200, kTagPage);
    fx.fake().route("/tasks/task_1/tags/tag_9", 200, taskWithTags());

    const RunResult result =
        runApp({"astral", "todo", "tag", "attach", "task_1", "auth", "--json"});
    REQUIRE(result.exitCode == 0);

    const json payload = json::parse(result.out);
    REQUIRE(payload.at("tags").at(0).at("id") == "tag_9");

    const client::HttpRequest& attach = fx.fake().requests.back();
    REQUIRE(attach.method == "PUT");
    REQUIRE(attach.url.find("/tasks/task_1/tags/tag_9") != std::string::npos);
    REQUIRE(attach.body.empty()); // idempotent attach sends no body
}

TEST_CASE("todo tag attach with a tag_ id skips the dictionary lookup") {
    ApiFixture fx;
    fx.fake().route("/tasks/task_1/tags/tag_9", 200, taskWithTags());

    const RunResult result =
        runApp({"astral", "todo", "tag", "attach", "task_1", "tag_9", "--json"});
    REQUIRE(result.exitCode == 0);
    REQUIRE(fx.fake().requests.size() == 2); // well-known + PUT only

    REQUIRE(fx.fake().requests.back().url.find("/tasks/task_1/tags/tag_9") != std::string::npos);
}

TEST_CASE("todo tag attach human output names the resolved tag") {
    ApiFixture fx;
    fx.fake().route("/workspaces/ws_1/tags", 200, kTagPage);
    fx.fake().route("/tasks/task_1/tags/tag_9", 200, taskWithTags());

    const RunResult result = runApp({"astral", "todo", "tag", "attach", "task_1", "auth"});
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.out.find("Attached auth to task_1") == 0);
    REQUIRE(result.out.find('\x1b') == std::string::npos);
}

TEST_CASE("todo tag attach with an unknown name is a local NotFound") {
    ApiFixture fx;
    fx.fake().route("/workspaces/ws_1/tags", 200, kTagPage);

    const RunResult result =
        runApp({"astral", "todo", "tag", "attach", "task_1", "nope", "--json"});
    REQUIRE(result.exitCode == static_cast<int>(astral::core::ExitCode::NotFound));

    const json payload = json::parse(result.out);
    REQUIRE(payload.at("error").at("code") == "NOT_FOUND");
}

TEST_CASE("todo tag detach deletes idempotently and confirms with one object") {
    ApiFixture fx;
    fx.fake().route("/workspaces/ws_1/tags", 200, kTagPage);
    fx.fake().route("/tasks/task_1/tags/tag_9", 204, json::object()); // empty 204 body

    const RunResult result =
        runApp({"astral", "todo", "tag", "detach", "task_1", "auth", "--json"});
    REQUIRE(result.exitCode == 0);

    const json payload = json::parse(result.out);
    REQUIRE(payload.at("detached") == true);
    REQUIRE(payload.at("task_id") == "task_1");
    REQUIRE(payload.at("tag_id") == "tag_9");

    const client::HttpRequest& detach = fx.fake().requests.back();
    REQUIRE(detach.method == "DELETE");
    REQUIRE(detach.url.find("/tasks/task_1/tags/tag_9") != std::string::npos);
}

TEST_CASE("todo tag detach human output names the resolved tag") {
    ApiFixture fx;
    fx.fake().route("/workspaces/ws_1/tags", 200, kTagPage);
    fx.fake().route("/tasks/task_1/tags/tag_9", 204, json::object());

    const RunResult result = runApp({"astral", "todo", "tag", "detach", "task_1", "auth"});
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.out.find("Detached auth from task_1") == 0);
}
} // namespace

// ---- v2.4 task batch (task-trees / move / batch done) --------------------

TEST_CASE("todo move posts parent_id three-state via PATCH") {
    ApiFixture fx;
    json moved = kTaskOne;
    moved["parent_id"] = "task_2";
    moved["revision"] = 4;
    fx.fake().route("/tasks/task_1", 200, kTaskOne); // revision read
    fx.fake().route("/tasks/task_1", 200, moved);    // PATCH reply

    const RunResult result = runApp({"astral", "todo", "move", "task_1", "--to", "task_2"});
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.out.find("Moved task_1 -> task_2") != std::string::npos);

    const client::HttpRequest& patch = fx.fake().requests.back();
    REQUIRE(patch.method == "PATCH");
    const json body = json::parse(patch.body);
    REQUIRE(body.at("parent_id") == "task_2");
    REQUIRE(body.at("expected_revision") == 3);
}

TEST_CASE("todo move --to - moves back to the workspace root with null") {
    ApiFixture fx;
    json rooted = kTaskOne;
    rooted["parent_id"] = nullptr;
    fx.fake().route("/tasks/task_1", 200, kTaskOne); // revision read
    fx.fake().route("/tasks/task_1", 200, rooted);   // PATCH reply

    const RunResult result = runApp({"astral", "todo", "move", "task_1", "--to", "-"});
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.out.find("workspace root") != std::string::npos);

    const json body = json::parse(fx.fake().requests.back().body);
    REQUIRE(body.at("parent_id").is_null());
}

TEST_CASE("todo done with multiple ids batches through batch-update") {
    ApiFixture fx;
    json taskB = kTaskOne;
    taskB["id"] = "task_2";
    fx.fake().route("/tasks/task_1", 200, kTaskOne); // revision read A
    fx.fake().route("/tasks/task_2", 200, taskB);    // revision read B
    json batch = json{{"items", json::array({kTaskOne, taskB})}};
    fx.fake().route("/workspaces/ws_1/tasks/batch-update", 200, batch);

    const RunResult result = runApp({"astral", "todo", "done", "task_1", "task_2", "--json"});
    REQUIRE(result.exitCode == 0);

    const json payload = json::parse(result.out);
    REQUIRE(payload.at("items").size() == 2);

    const client::HttpRequest& post = fx.fake().requests.back();
    REQUIRE(post.method == "POST");
    REQUIRE(post.url.find("/workspaces/ws_1/tasks/batch-update") != std::string::npos);
    const json body = json::parse(post.body);
    REQUIRE(body.at("items").size() == 2);
    REQUIRE(body.at("items").at(0).at("task_id") == "task_1");
    REQUIRE(body.at("items").at(0).at("expected_revision") == 3);
    REQUIRE(body.at("set").at("status") == "done");
}

TEST_CASE("todo done with one id keeps the single PATCH path") {
    ApiFixture fx;
    json doneTask = kTaskOne;
    doneTask["status"] = "done";
    doneTask["revision"] = 4;
    fx.fake().route("/tasks/task_1", 200, kTaskOne); // revision read
    fx.fake().route("/tasks/task_1", 200, doneTask); // PATCH reply

    const RunResult result = runApp({"astral", "todo", "done", "task_1"});
    REQUIRE(result.exitCode == 0);
    const client::HttpRequest& patch = fx.fake().requests.back();
    REQUIRE(patch.method == "PATCH");
}

TEST_CASE("todo add-tree posts nested trees with a deterministic idempotency key") {
    ApiFixture fx;
    const json treeIn =
        json{{"trees", json::array({json{{"title", "Epic"},
                                         {"children", json::array({json{{"title", "Story"}}})}},
                                    json{{"title", "Solo"}}})}};
    const json batchOut = json{
        {"items",
         json::array({
             json{
                 {"task", json{{"id", "tsk_a"}, {"title", "Epic"}}},
                 {"children", json::array({json{{"task", json{{"id", "tsk_b"}, {"title", "Story"}}},
                                                {"children", json::array()}}})}},
             json{{"task", json{{"id", "tsk_c"}, {"title", "Solo"}}}, {"children", json::array()}},
         })}};
    fx.fake().route("/workspaces/ws_1/task-trees", 201, batchOut, /*uses=*/2); // 首发 + 幂等重放

    const auto file = fx.workDir() / "tree.json";
    {
        std::ofstream out(file, std::ios::binary);
        out << treeIn.dump();
    }

    const RunResult result = runApp({"astral", "todo", "add-tree", "--file", file.string()});
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.out.find("Created 3 task(s) in 2 tree(s)") != std::string::npos);
    REQUIRE(result.out.find("- tsk_a  Epic") != std::string::npos);
    REQUIRE(result.out.find("    - tsk_b  Story") != std::string::npos);

    const client::HttpRequest& post = fx.fake().requests.back();
    REQUIRE(post.method == "POST");
    REQUIRE(post.url.find("/workspaces/ws_1/task-trees") != std::string::npos);
    const json body = json::parse(post.body);
    REQUIRE(body.at("trees").size() == 2);
    REQUIRE(body.at("trees").at(0).at("children").at(0).at("title") == "Story");

    // Idempotency-Key: present, deterministic prefix, stable across reruns.
    std::string keyA;
    for (const auto& h : post.headers) {
        if (h.first == "Idempotency-Key") {
            keyA = h.second;
        }
    }
    REQUIRE(keyA.rfind("todo-tree-", 0) == 0);

    const RunResult rerun = runApp({"astral", "todo", "add-tree", "--file", file.string()});
    REQUIRE(rerun.exitCode == 0);
    std::string keyB;
    for (const auto& h : fx.fake().requests.back().headers) {
        if (h.first == "Idempotency-Key") {
            keyB = h.second;
        }
    }
    REQUIRE(keyA == keyB);
}

TEST_CASE("todo add-tree --parent targets the task container") {
    ApiFixture fx;
    fx.fake().route("/tasks/task_9/task-trees", 201, json{{"items", json::array()}});

    const auto file = fx.workDir() / "sub.json";
    {
        std::ofstream out(file, std::ios::binary);
        out << R"([{"title":"Sub"}])"; // bare array form
    }

    const RunResult result = runApp(
        {"astral", "todo", "add-tree", "--file", file.string(), "--parent", "task_9", "--json"});
    REQUIRE(result.exitCode == 0);

    const client::HttpRequest& post = fx.fake().requests.back();
    REQUIRE(post.url.find("/tasks/task_9/task-trees") != std::string::npos);
    const json body = json::parse(post.body);
    REQUIRE(body.at("trees").at(0).at("title") == "Sub");
}

TEST_CASE("todo add-tree with malformed input is a usage error without network") {
    ApiFixture fx;
    const auto file = fx.workDir() / "bad.json";
    {
        std::ofstream out(file, std::ios::binary);
        out << "{not json";
    }
    const RunResult result = runApp({"astral", "todo", "add-tree", "--file", file.string()});
    REQUIRE(result.exitCode == 2);           // Usage
    REQUIRE(fx.fake().requests.size() == 0); // parse/usage failure precedes any network

    const auto empty = fx.workDir() / "empty.json";
    {
        std::ofstream out(empty, std::ios::binary);
        out << "{\"trees\": []}";
    }
    const RunResult emptyRun = runApp({"astral", "todo", "add-tree", "--file", empty.string()});
    REQUIRE(emptyRun.exitCode == 2);
}

TEST_CASE("todo add carries a deterministic idempotency key") {
    ApiFixture fx;
    fx.fake().route("/workspaces/ws_1/children", 201, kTaskOne);

    const RunResult result = runApp({"astral", "todo", "add", "Fix login flow"});
    REQUIRE(result.exitCode == 0);

    const client::HttpRequest& post = fx.fake().requests.back();
    std::string key;
    for (const auto& h : post.headers) {
        if (h.first == "Idempotency-Key") {
            key = h.second;
        }
    }
    REQUIRE(key.rfind("todo-", 0) == 0);
}

// ---- v2.5 task dependencies (todo dep / show / --blocked) -----------------

TEST_CASE("todo dep add posts a blocks edge by default and relates with the flag") {
    ApiFixture fx;
    fx.fake().route("/tasks/task_1/dependencies/task_2", 201,
                    json{{"from_task_id", "task_1"}, {"to_task_id", "task_2"}, {"kind", "blocks"}});

    const RunResult result = runApp({"astral", "todo", "dep", "add", "task_1", "task_2"});
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.out.find("Blocked task_1 by task_2") != std::string::npos);

    const client::HttpRequest& put = fx.fake().requests.back();
    REQUIRE(put.method == "PUT");
    REQUIRE(put.url.find("/tasks/task_1/dependencies/task_2") != std::string::npos);
    REQUIRE(json::parse(put.body).at("kind") == "blocks");

    fx.fake().route("/tasks/task_1/dependencies/task_2", 201, json{{"kind", "relates"}});
    const RunResult rel = runApp({"astral", "todo", "dep", "add", "task_1", "task_2", "--relates"});
    REQUIRE(rel.exitCode == 0);
    REQUIRE(rel.out.find("Related task_1 <-> task_2") != std::string::npos);
    REQUIRE(json::parse(fx.fake().requests.back().body).at("kind") == "relates");
}

TEST_CASE("todo dep remove deletes with the kind query and confirms") {
    ApiFixture fx;
    fx.fake().route("/tasks/task_1/dependencies/task_2", 204, json::object());

    const RunResult result = runApp({"astral", "todo", "dep", "remove", "task_1", "task_2"});
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.out.find("Removed blocks edge task_1 -> task_2") != std::string::npos);

    const client::HttpRequest& del = fx.fake().requests.back();
    REQUIRE(del.method == "DELETE");
    REQUIRE(del.url.find("kind=blocks") != std::string::npos);
}

TEST_CASE("todo dep list prints edges both ways and supports --json") {
    ApiFixture fx;
    const json list = json{{"items", json::array({
                                         json{{"from_task_id", "task_1"},
                                              {"to_task_id", "task_2"},
                                              {"kind", "blocks"},
                                              {"created_at", "2026-09-30T00:00:00Z"}},
                                         json{{"from_task_id", "task_3"},
                                              {"to_task_id", "task_1"},
                                              {"kind", "relates"},
                                              {"created_at", "2026-09-30T00:00:00Z"}},
                                     })}};
    fx.fake().route("/tasks/task_1/dependencies", 200, list);

    const RunResult result = runApp({"astral", "todo", "dep", "list", "task_1"});
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.out.find("task_1 -> task_2  (blocks)") != std::string::npos);
    REQUIRE(result.out.find("task_3 -> task_1  (relates)") != std::string::npos);

    fx.fake().route("/tasks/task_1/dependencies", 200, list);
    const RunResult asJson = runApp({"astral", "todo", "dep", "list", "task_1", "--json"});
    REQUIRE(asJson.exitCode == 0);
    REQUIRE(json::parse(asJson.out).at("items").size() == 2);
}

TEST_CASE("todo show renders dependency view fields") {
    ApiFixture fx;
    json detailed = kTaskOne;
    detailed["blocked_by"] = json::array({"task_9"});
    detailed["blocks"] = json::array();
    detailed["related"] = json::array({"task_7", "task_8"});
    fx.fake().route("/tasks/task_1", 200, detailed);

    const RunResult result = runApp({"astral", "todo", "show", "task_1"});
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.out.find("blocked_by:") != std::string::npos);
    REQUIRE(result.out.find("task_9") != std::string::npos);
    REQUIRE(result.out.find("related:") != std::string::npos);
    REQUIRE(result.out.find("task_7, task_8") != std::string::npos);
}

TEST_CASE("todo list forwards blocked filters as query parameters") {
    ApiFixture fx;
    fx.fake().route("/workspaces/ws_1/children", 200, kTaskPage);

    const RunResult result =
        runApp({"astral", "todo", "list", "--blocked", "--blocked-by", "task_2", "--json"});
    REQUIRE(result.exitCode == 0);

    const client::HttpRequest& req = fx.fake().requests.back();
    REQUIRE(req.url.find("blocked=true") != std::string::npos);
    REQUIRE(req.url.find("blocked_by=task_2") != std::string::npos);
}
