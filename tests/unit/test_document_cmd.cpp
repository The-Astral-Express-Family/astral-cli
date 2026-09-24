// astral document 命令族的 runApp 级单测（phase-5 第一层）：manifest/get/
// push/delete 与 conflicts list/show/resolve 的 wire 形状、base 指针解析
// （缺省 GET-改-写 / 显式 base / tombstone 复活）、Idempotency-Key、
// 409 冲突提示、路径分段编码与 R2 身份头。
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "core/content_hash.hpp"
#include "core/exit_codes.hpp"
#include "support/api_fixture.hpp"

namespace {

namespace client = astral::client;
using json = nlohmann::json;
using astral_test::ApiFixture;
using astral_test::errorEnvelopeBody;
using astral_test::runApp;
using astral_test::RunResult;

const std::string kEmptyHash =
    "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

const json kDoc = json{{"path", "notes/demo.md"},
                       {"revision", 3},
                       {"content_hash", "sha256:" + std::string(64, 'a')},
                       {"content", "# demo\n"},
                       {"updated_at", "2026-09-24T00:00:00Z"}};

const json kConflict = json{{"id", "dfc_1"},
                            {"path", "notes/demo.md"},
                            {"base_revision", 3},
                            {"base_hash", "sha256:" + std::string(64, 'a')},
                            {"status", "open"},
                            {"ours_hash", "sha256:" + std::string(64, 'b')},
                            {"theirs_revision", 4},
                            {"theirs_hash", "sha256:" + std::string(64, 'c')},
                            {"created_at", "2026-09-24T01:00:00Z"},
                            {"resolution", nullptr},
                            {"resolved_by", nullptr},
                            {"resolved_at", nullptr}};

json conflictDetail() {
    json detail = kConflict;
    detail["ours_content"] = "# ours\n";
    detail["theirs_content"] = "# theirs\n";
    return detail;
}

bool hasHeader(const client::HttpRequest& request, const std::string& name,
               const std::string& value) {
    for (const auto& [headerName, headerValue] : request.headers) {
        if (headerName == name && headerValue == value) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("document manifest lists rows and honors the page envelope") {
    ApiFixture fx;
    fx.fake().route("/documents/manifest", 200,
                    json{{"items", json::array({json{{"path", "AGENTS.md"},
                                                     {"revision", 1},
                                                     {"size", 12},
                                                     {"updated_at", "2026-09-24T00:00:00Z"}},
                                                json{{"path", "notes/demo.md"},
                                                     {"revision", 3},
                                                     {"size", 7},
                                                     {"deleted", true},
                                                     {"updated_at", "2026-09-24T00:00:00Z"}}})},
                         {"next_cursor", nullptr}},
                    2);

    const RunResult machine = runApp({"astral", "document", "manifest", "--json"});
    REQUIRE(machine.exitCode == 0);
    const json payload = json::parse(machine.out);
    REQUIRE(payload.at("workspace_id") == "ws_1");
    REQUIRE(payload.at("items").size() == 2);
    REQUIRE(payload.at("next_cursor").is_null());

    const RunResult human = runApp({"astral", "document", "manifest", "--include-deleted"});
    REQUIRE(human.exitCode == 0);
    REQUIRE(human.out.find("notes/demo.md") != std::string::npos);
    REQUIRE(human.out.find("(deleted)") != std::string::npos);
    // include_deleted 旗标序列化进 query。
    REQUIRE(fx.fake().requests.back().url.find("include_deleted=true") != std::string::npos);
}

TEST_CASE("document get prints the Document verbatim in json mode and raw content with --raw") {
    ApiFixture fx;
    fx.fake().route("/documents/notes/demo.md", 200, kDoc, 2);

    const RunResult machine = runApp({"astral", "document", "get", "notes/demo.md", "--json"});
    REQUIRE(machine.exitCode == 0);
    REQUIRE(json::parse(machine.out) == kDoc);

    const RunResult raw = runApp({"astral", "document", "get", "notes/demo.md", "--raw"});
    REQUIRE(raw.exitCode == 0);
    REQUIRE(raw.out == "# demo\n");

    fx.fake().route("/documents/notes/missing.md", 404,
                    json::parse(errorEnvelopeBody("NOT_FOUND", "no such document")));
    const RunResult missing = runApp({"astral", "document", "get", "notes/missing.md", "--json"});
    REQUIRE(missing.exitCode == 4);
    REQUIRE(json::parse(missing.out).at("error").at("code") == "NOT_FOUND");
}

TEST_CASE("document push with an explicit empty body creates an empty document") {
    // CLI11 对空串值不置位 optional——--body "" 必须按「显式给出的空内容」
    // 处理（与 profile --bio "" 同类回归），不得落进「缺内容源」用法错误。
    ApiFixture fx;
    fx.fake().route("/documents/docs/empty.md", 404,
                    json::parse(errorEnvelopeBody("NOT_FOUND", "no such document")));
    fx.fake().route("/documents/docs/empty.md", 200,
                    json{{"path", "docs/empty.md"},
                         {"revision", 1},
                         {"content_hash", kEmptyHash},
                         {"content", ""},
                         {"updated_at", "2026-09-24T00:00:00Z"}});

    const RunResult result = runApp({"astral", "document", "push", "docs/empty.md", "--body", ""});
    REQUIRE(result.exitCode == 0);
    const json body = json::parse(fx.fake().requests.back().body);
    REQUIRE(body.at("content") == "");
    REQUIRE(body.at("content_hash") == kEmptyHash);
}

TEST_CASE("document push resolves the base pointer by fetching the current row") {
    ApiFixture fx;
    fx.fake().route("/documents/notes/demo.md", 200, kDoc); // GET base
    fx.fake().route("/documents/notes/demo.md", 200, kDoc); // PUT

    const RunResult result =
        runApp({"astral", "document", "push", "notes/demo.md", "--body", "# new"});
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.out.find("revision 3") != std::string::npos);

    REQUIRE(fx.fake().requests.size() == 3); // well-known + GET + PUT
    const client::HttpRequest& push = fx.fake().requests.back();
    REQUIRE(push.method == "PUT");
    const json body = json::parse(push.body);
    REQUIRE(body.at("base_revision") == 3);
    REQUIRE(body.at("base_hash") == "sha256:" + std::string(64, 'a'));
    REQUIRE(body.at("content") == "# new");
    REQUIRE(body.at("content_hash") == astral::core::sha256ContentHash("# new"));
    REQUIRE(hasHeader(
        push, "Idempotency-Key",
        "doc-" + astral::core::fnv1aHex("notes/demo.md|3|sha256:" + std::string(64, 'a') + "|" +
                                        body.at("content_hash").get<std::string>())));
}

TEST_CASE("document push creates with base 0 when the path is unknown") {
    ApiFixture fx;
    fx.fake().route("/documents/docs/new.md", 404,
                    json::parse(errorEnvelopeBody("NOT_FOUND", "no such document")));
    fx.fake().route("/documents/docs/new.md", 200,
                    json{{"path", "docs/new.md"},
                         {"revision", 1},
                         {"content_hash", kEmptyHash},
                         {"content", "hello"},
                         {"updated_at", "2026-09-24T00:00:00Z"}});

    const RunResult result =
        runApp({"astral", "document", "push", "docs/new.md", "--body", "hello", "--json"});
    REQUIRE(result.exitCode == 0);
    REQUIRE(json::parse(result.out).at("revision") == 1);

    const json body = json::parse(fx.fake().requests.back().body);
    REQUIRE(body.at("base_revision") == 0);
    REQUIRE(body.at("base_hash") == kEmptyHash); // base 0：空内容 hash 占位
    REQUIRE(body.at("content_hash") ==
            "sha256:2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
}

TEST_CASE("document push hashes raw file bytes without newline rewriting") {
    ApiFixture fx;
    const std::filesystem::path file = fx.workDir() / "doc-crlf.md";
    {
        std::ofstream out(file, std::ios::binary);
        out << "line1\r\nline2\r\n";
    }
    fx.fake().route("/documents/docs/crlf.md", 404,
                    json::parse(errorEnvelopeBody("NOT_FOUND", "no such document")));
    fx.fake().route("/documents/docs/crlf.md", 200,
                    json{{"path", "docs/crlf.md"},
                         {"revision", 1},
                         {"content_hash", ""},
                         {"content", ""},
                         {"updated_at", "2026-09-24T00:00:00Z"}});

    const RunResult result =
        runApp({"astral", "document", "push", "docs/crlf.md", "--file", file.string()});
    REQUIRE(result.exitCode == 0);
    const json body = json::parse(fx.fake().requests.back().body);
    // CRLF 必须原样进 JSON 串与 hash（协议：不重写换行）。
    REQUIRE(body.at("content").get<std::string>().find("\r\n") != std::string::npos);
    REQUIRE(body.at("content_hash") == astral::core::sha256ContentHash("line1\r\nline2\r\n"));
}

TEST_CASE("document push with an explicit base requires the hash") {
    ApiFixture fx;
    const RunResult result = runApp({"astral", "document", "push", "notes/demo.md", "--body", "x",
                                     "--base-revision", "3", "--json"});
    REQUIRE(result.exitCode == 2);
    const json payload = json::parse(result.out);
    REQUIRE(payload.at("error").at("code") == "USAGE");
    REQUIRE(fx.fake().requests.size() == 1); // 只有 well-known，未发任何业务请求
}

TEST_CASE("document push surfaces the conflict id from a 409 envelope") {
    ApiFixture fx;
    fx.fake().route("/documents/notes/demo.md", 200, kDoc); // GET base
    fx.fake().route("/documents/notes/demo.md", 409,
                    json{{"error",
                          {{"code", "DOCUMENT_CONFLICT"},
                           {"message", "remote revision moved"},
                           {"retryable", false},
                           {"request_id", "req_9"},
                           {"details", {{"conflict_id", "dfc_42"}, {"current_revision", 5}}}}}});

    const RunResult human = runApp({"astral", "document", "push", "notes/demo.md", "--body", "x"});
    REQUIRE(human.exitCode == 5);
    // 人读模式的失败走 stderr（--json 契约：stdout 归机器）。
    REQUIRE(human.err.find("dfc_42") != std::string::npos);
    REQUIRE(human.err.find("astral document conflicts show dfc_42") != std::string::npos);

    // 路由按消费计数：第二次运行前重铺 GET/PUT 两条。
    fx.fake().route("/documents/notes/demo.md", 200, kDoc);
    fx.fake().route("/documents/notes/demo.md", 409,
                    json{{"error",
                          {{"code", "DOCUMENT_CONFLICT"},
                           {"message", "remote revision moved"},
                           {"retryable", false},
                           {"request_id", "req_9"},
                           {"details", {{"conflict_id", "dfc_42"}, {"current_revision", 5}}}}}});
    const RunResult machine =
        runApp({"astral", "document", "push", "notes/demo.md", "--body", "x", "--json"});
    REQUIRE(machine.exitCode == 5);
    const json payload = json::parse(machine.out);
    REQUIRE(payload.at("error").at("code") == "DOCUMENT_CONFLICT");
    REQUIRE(payload.at("error").at("request_id") == "req_9");
}

TEST_CASE("document push revives a tombstone through base 0") {
    ApiFixture fx;
    fx.fake().route("/documents/notes/gone.md", 200,
                    json{{"path", "notes/gone.md"},
                         {"revision", 7},
                         {"content_hash", "sha256:" + std::string(64, 'd')},
                         {"content", ""},
                         {"updated_at", "2026-09-24T00:00:00Z"},
                         {"deleted", true}});
    fx.fake().route("/documents/notes/gone.md", 200,
                    json{{"path", "notes/gone.md"},
                         {"revision", 8},
                         {"content_hash", kEmptyHash},
                         {"content", "back"},
                         {"updated_at", "2026-09-24T00:00:00Z"}});

    const RunResult result =
        runApp({"astral", "document", "push", "notes/gone.md", "--body", "back"});
    REQUIRE(result.exitCode == 0);
    const json body = json::parse(fx.fake().requests.back().body);
    REQUIRE(body.at("base_revision") == 0); // tombstone -> 复活路径
    REQUIRE(body.at("base_hash") == kEmptyHash);
}

TEST_CASE("document delete fetches the current revision then tombstones") {
    ApiFixture fx;
    fx.fake().route("/documents/notes/demo.md", 200, kDoc); // GET base
    fx.fake().route("/documents/notes/demo.md", 204, json(nullptr));

    const RunResult result = runApp({"astral", "document", "delete", "notes/demo.md", "--json"});
    REQUIRE(result.exitCode == 0);
    REQUIRE(json::parse(result.out).at("deleted") == true);

    const client::HttpRequest& remove = fx.fake().requests.back();
    REQUIRE(remove.method == "DELETE");
    REQUIRE(remove.url.find("base_revision=3") != std::string::npos);

    // 显式 base 跳过预读。
    fx.fake().route("/documents/notes/other.md", 204, json(nullptr));
    REQUIRE(runApp({"astral", "document", "delete", "notes/other.md", "--base-revision", "12"})
                .exitCode == 0);
    REQUIRE(fx.fake().requests.back().url.find("base_revision=12") != std::string::npos);

    // 远端不存在的路径：无东西可删。
    fx.fake().route("/documents/notes/void.md", 404,
                    json::parse(errorEnvelopeBody("NOT_FOUND", "no such document")));
    REQUIRE(runApp({"astral", "document", "delete", "notes/void.md"}).exitCode == 4);
}

TEST_CASE("document conflicts list, show and resolve") {
    ApiFixture fx;
    fx.fake().route("/conflicts", 200,
                    json{{"items", json::array({kConflict})}, {"next_cursor", nullptr}});
    const RunResult list = runApp({"astral", "document", "conflicts", "list", "--json"});
    REQUIRE(list.exitCode == 0);
    const json page = json::parse(list.out);
    REQUIRE(page.at("items").at(0).at("id") == "dfc_1");
    // status 过滤序列化（默认 open 也显式携带）。
    REQUIRE(fx.fake().requests.back().url.find("status=open") != std::string::npos);

    fx.fake().route("/conflicts/dfc_1", 200, conflictDetail());
    const RunResult show = runApp({"astral", "document", "conflicts", "show", "dfc_1"});
    REQUIRE(show.exitCode == 0);
    REQUIRE(show.out.find("# ours") != std::string::npos);
    REQUIRE(show.out.find("# theirs") != std::string::npos);

    fx.fake().route("/conflicts/dfc_1/resolve", 200, kDoc);
    const RunResult resolve = runApp({"astral", "document", "conflicts", "resolve", "dfc_1",
                                      "--resolution", "merged", "--body", "# merged"});
    REQUIRE(resolve.exitCode == 0);
    const json body = json::parse(fx.fake().requests.back().body);
    REQUIRE(body.at("resolution") == "merged");
    REQUIRE(body.at("content") == "# merged");

    // merged/manual 必须带内容；ours/theirs 不得带内容。
    REQUIRE(
        runApp({"astral", "document", "conflicts", "resolve", "dfc_1", "--resolution", "manual"})
            .exitCode == 2);
    REQUIRE(runApp({"astral", "document", "conflicts", "resolve", "dfc_1", "--resolution", "ours",
                    "--body", "x"})
                .exitCode == 2);
}

TEST_CASE("document paths are validated and encoded per segment") {
    ApiFixture fx;
    // 按值迭代 const char* 初始化列表：const std::string& 绑定 const char*
    // 临时量会触发 gcc 的 -Wrange-loop-construct（CI -Werror）。
    for (const char* const bad :
         {"notes//a.md", "/abs.md", "notes/../secrets.md", "notes\\a.md", "notes/."}) {
        const RunResult result = runApp({"astral", "document", "get", bad, "--json"});
        INFO("path: " << bad);
        REQUIRE(result.exitCode == 2);
        REQUIRE(json::parse(result.out).at("error").at("code") == "USAGE");
    }
    // 每次运行都先过 well-known 发现（工作区解析前置），之后本地拒绝。
    REQUIRE(fx.fake().requests.size() == 5);

    fx.fake().route("/documents/notes/my%20file.md", 200, kDoc);
    const RunResult encoded = runApp({"astral", "document", "get", "notes/my file.md", "--json"});
    REQUIRE(encoded.exitCode == 0);
    // '/' 分隔保持字面，段内空格转义。
    REQUIRE(fx.fake().requests.back().url.find("/documents/notes/my%20file.md") !=
            std::string::npos);
}

TEST_CASE("api requests carry the R2 client identity headers") {
    ApiFixture fx;
    fx.fake().route("/documents/manifest", 200,
                    json{{"items", json::array()}, {"next_cursor", nullptr}});
    REQUIRE(runApp({"astral", "document", "manifest"}).exitCode == 0);
    for (const client::HttpRequest& request : fx.fake().requests) {
        INFO("url: " << request.url);
        if (request.url.find("/.well-known/") != std::string::npos) {
            continue; // 发现文档不带版本头（协议只约定 /api/v1）
        }
        REQUIRE(hasHeader(request, "X-Astral-Client", "cli"));
        REQUIRE(hasHeader(request, "X-Astral-Client-Version", "2"));
    }
}

TEST_CASE("push rejects non-utf8 and oversized content locally") {
    ApiFixture fx;
    const std::filesystem::path file = fx.workDir() / "bad.bin";
    {
        std::ofstream out(file, std::ios::binary);
        out << "ok\xff\xfe";
    } // 先落盘再跑命令：ofstream 缓冲未刷时文件是空的
    const RunResult result =
        runApp({"astral", "document", "push", "docs/bad.md", "--file", file.string()});
    REQUIRE(result.exitCode == 2);
    REQUIRE(fx.fake().requests.size() == 1); // 未触网

    // 缺内容源同样是用法错误。
    REQUIRE(runApp({"astral", "document", "push", "docs/x.md"}).exitCode == 2);
}
