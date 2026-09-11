// astral tags / astral msg 命令族的 runApp 级单测（round 19）：
// 两步确认流程（propose → 打印可复制确认命令 → confirm）、rename/delete 的
// target 解析、msg 目标语法与容器端点选择、Idempotency-Key 确定性。
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/exit_codes.hpp"
#include "support/api_fixture.hpp"

namespace {

namespace client = astral::client;
using json = nlohmann::json;
using astral_test::ApiFixture;
using astral_test::runApp;
using astral_test::RunResult;

const json kTag = json{{"id", "tag_1"}, {"workspace_id", "ws_1"}, {"name", "backend"}};

const json kProposal = json{
    {"proposal_id", "tgp_1"},
    {"action", "create"},
    {"name", "backend"},
    {"confirm_code", "ABCD-1234"},
    {"expires_at", "2026-09-10T12:00:00Z"},
    {"existing_tags",
     json::array({json{{"id", "tag_0"}, {"workspace_id", "ws_1"}, {"name", "auth"}}})},
};

const json kMessage = json{
    {"id", "msg_1"},        {"workspace_id", "ws_1"}, {"thread_id", nullptr},
    {"sender_id", "usr_1"}, {"body", "hello"},        {"created_at", "2026-09-10T00:00:00Z"},
};

TEST_CASE("tags list prints the workspace dictionary") {
    ApiFixture fx;
    fx.fake().route("/workspaces/ws_1/tags", 200,
                    json{{"items", json::array({kTag})}, {"next_cursor", nullptr}});

    const RunResult result = runApp({"astral", "tags", "list", "--json"});
    REQUIRE(result.exitCode == 0);

    const json payload = json::parse(result.out);
    REQUIRE(payload.at("workspace_id") == "ws_1");
    REQUIRE(payload.at("items").size() == 1);
    REQUIRE(payload.at("items").at(0).at("name") == "backend");
    REQUIRE(payload.at("next_cursor").is_null());
    REQUIRE(fx.fake().requests.size() == 2); // well-known + tags
}

TEST_CASE("tags create proposes and prints a copy-pasteable confirm command") {
    ApiFixture fx;
    fx.fake().route("/tag-proposals", 201, kProposal, 2);

    const RunResult human = runApp({"astral", "tags", "create", "Backend"});
    REQUIRE(human.exitCode == 0);
    REQUIRE(human.out.find("tgp_1") != std::string::npos);
    REQUIRE(human.out.find("ABCD-1234") != std::string::npos);
    REQUIRE(human.out.find("astral tags create Backend --proposal tgp_1 --confirm ABCD-1234") !=
            std::string::npos);

    const RunResult machine = runApp({"astral", "tags", "create", "Backend", "--json"});
    REQUIRE(machine.exitCode == 0);
    const json payload = json::parse(machine.out);
    REQUIRE(payload.at("proposal_id") == "tgp_1");
    REQUIRE(payload.at("confirm_code") == "ABCD-1234");
    REQUIRE(payload.at("existing_tags").is_array());

    const client::HttpRequest& request = fx.fake().requests.back();
    const json body = json::parse(request.body);
    REQUIRE(body.at("action") == "create");
    REQUIRE(body.at("name") == "Backend");
    REQUIRE_FALSE(body.contains("target_tag_id"));
}

TEST_CASE("tags create --proposal --confirm posts the confirm body") {
    ApiFixture fx;
    fx.fake().route("/tag-proposals/tgp_1/confirm", 200, kTag);

    const RunResult result = runApp({"astral", "tags", "create", "Backend", "--proposal", "tgp_1",
                                     "--confirm", "ABCD-1234", "--json"});
    REQUIRE(result.exitCode == 0);

    const json payload = json::parse(result.out);
    REQUIRE(payload.at("id") == "tag_1");

    const client::HttpRequest& request = fx.fake().requests.back();
    REQUIRE(request.url.find("/tag-proposals/tgp_1/confirm") != std::string::npos);
    const json body = json::parse(request.body);
    REQUIRE(body.at("confirm_code") == "ABCD-1234");
    REQUIRE(body.at("name") == "Backend");
}

TEST_CASE("tags rename resolves the target by name and proposes with its id") {
    ApiFixture fx;
    fx.fake().route("/workspaces/ws_1/tags", 200,
                    json{{"items", json::array({kTag})}, {"next_cursor", nullptr}});
    fx.fake().route("/tag-proposals", 201, kProposal, 2);

    const RunResult result = runApp({"astral", "tags", "rename", "Backend", "api-core", "--json"});
    REQUIRE(result.exitCode == 0);

    // Name resolution rides the dictionary endpoint before the proposal.
    const client::HttpRequest& proposalRequest = fx.fake().requests.back();
    const json body = json::parse(proposalRequest.body);
    REQUIRE(body.at("action") == "rename");
    REQUIRE(body.at("name") == "api-core");
    REQUIRE(body.at("target_tag_id") == "tag_1");
}

TEST_CASE("tags delete confirms with the canonical target name") {
    ApiFixture fx;
    fx.fake().route("/workspaces/ws_1/tags", 200,
                    json{{"items", json::array({kTag})}, {"next_cursor", nullptr}});
    fx.fake().route("/tag-proposals/tgp_9/confirm", 200, kTag);

    const RunResult result = runApp({"astral", "tags", "delete", "backend", "--proposal", "tgp_9",
                                     "--confirm", "ZZZZ-999", "--json"});
    REQUIRE(result.exitCode == 0);

    const json body = json::parse(fx.fake().requests.back().body);
    REQUIRE(body.at("name") == "backend");
}

TEST_CASE("tags unknown name resolves to a not-found error") {
    ApiFixture fx;
    fx.fake().route("/workspaces/ws_1/tags", 200,
                    json{{"items", json::array()}, {"next_cursor", nullptr}});

    const RunResult result = runApp({"astral", "tags", "delete", "ghost", "--json"});
    REQUIRE(result.exitCode == static_cast<int>(astral::core::ExitCode::NotFound));
    REQUIRE(json::parse(result.out).at("error").at("code") == "NOT_FOUND");
}

TEST_CASE("msg send parses workspace/actor/task targets") {
    ApiFixture fx;
    fx.fake().route("/workspaces/ws_1/messages", 201, kMessage, 3);

    const RunResult ws = runApp({"astral", "msg", "send", "workspace", "hello", "--json"});
    REQUIRE(ws.exitCode == 0);
    json body = json::parse(fx.fake().requests.back().body);
    REQUIRE(body.at("target") == json{{"type", "workspace"}, {"id", "ws_1"}});
    REQUIRE(body.at("body") == "hello");
    REQUIRE_FALSE(body.contains("thread_id"));
    // Idempotency-Key rides the header and is content-deterministic.
    bool hasKey = false;
    std::string keyValue;
    for (const auto& [key, value] : fx.fake().requests.back().headers) {
        if (key == "Idempotency-Key") {
            hasKey = true;
            keyValue = value;
        }
    }
    REQUIRE(hasKey);
    REQUIRE(keyValue.rfind("msg-", 0) == 0);

    const RunResult actor = runApp({"astral", "msg", "send", "actor:usr_9", "dm", "--json"});
    REQUIRE(actor.exitCode == 0);
    body = json::parse(fx.fake().requests.back().body);
    REQUIRE(body.at("target") == json{{"type", "actor"}, {"id", "usr_9"}});

    const RunResult task = runApp({"astral", "msg", "send", "task:task_5", "update"});
    REQUIRE(task.exitCode == 0);
    body = json::parse(fx.fake().requests.back().body);
    REQUIRE(body.at("target") == json{{"type", "task"}, {"id", "task_5"}});
}

TEST_CASE("msg send --thread includes the thread id") {
    ApiFixture fx;
    fx.fake().route("/workspaces/ws_1/messages", 201, kMessage);

    const RunResult result =
        runApp({"astral", "msg", "send", "task:task_5", "update", "--thread", "msg_1"});
    REQUIRE(result.exitCode == 0);
    const json body = json::parse(fx.fake().requests.back().body);
    REQUIRE(body.at("thread_id") == "msg_1");
}

TEST_CASE("msg send rejects malformed targets as usage errors") {
    ApiFixture fx;

    const RunResult bogus = runApp({"astral", "msg", "send", "bogus", "hi", "--json"});
    REQUIRE(bogus.exitCode == static_cast<int>(astral::core::ExitCode::Usage));
    REQUIRE(json::parse(bogus.out).at("error").at("code") == "USAGE");

    const RunResult noId = runApp({"astral", "msg", "send", "actor:", "hi"});
    REQUIRE(noId.exitCode == static_cast<int>(astral::core::ExitCode::Usage));
}

TEST_CASE("msg list selects the container by flag") {
    ApiFixture fx;

    SECTION("--task uses the task thread collection") {
        fx.fake().route("/tasks/task_5/messages", 200,
                        json{{"items", json::array({kMessage})}, {"next_cursor", nullptr}});
        const RunResult result = runApp({"astral", "msg", "list", "--task", "task_5", "--json"});
        REQUIRE(result.exitCode == 0);
        const json payload = json::parse(result.out);
        REQUIRE(payload.at("items").size() == 1);
        bool sawTaskThread = false;
        for (const auto& request : fx.fake().requests) {
            if (request.url.find("/tasks/task_5/messages") != std::string::npos) {
                sawTaskThread = true;
            }
        }
        REQUIRE(sawTaskThread);
    }

    SECTION("--thread filters the workspace collection") {
        fx.fake().route("/workspaces/ws_1/messages", 200,
                        json{{"items", json::array()}, {"next_cursor", nullptr}});
        const RunResult result = runApp({"astral", "msg", "list", "--thread", "msg_1", "--json"});
        REQUIRE(result.exitCode == 0);
        std::string url;
        for (const auto& request : fx.fake().requests) {
            if (request.url.find("/workspaces/ws_1/messages") != std::string::npos) {
                url = request.url;
            }
        }
        REQUIRE(url.find("thread_id=msg_1") != std::string::npos);
    }

    SECTION("bare list hits the workspace collection") {
        fx.fake().route("/workspaces/ws_1/messages", 200,
                        json{{"items", json::array()}, {"next_cursor", nullptr}});
        const RunResult result = runApp({"astral", "msg", "list"});
        REQUIRE(result.exitCode == 0);
        REQUIRE(result.out.find("No messages.") == 0);
    }
}

} // namespace
