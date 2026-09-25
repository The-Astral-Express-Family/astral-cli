#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "core/version.hpp"

// The snapshot under test mirrors the discovery document shape from
// ARCHITECTURE.md section 5; when astral-modulator publishes new snapshots,
// the required-field assertions here are the first thing to reconcile.
TEST_CASE("v2 discovery snapshot keeps its promised fields") {
    std::ifstream input(std::string(ASTRAL_PROTOCOL_DIR) + "/snapshots/v2/well-known.json");
    REQUIRE(input.good());

    const auto doc = nlohmann::json::parse(input);
    REQUIRE(doc.at("server_id").is_string());
    REQUIRE(doc.at("canonical_url").is_string());
    REQUIRE(doc.at("api_base").is_string());
    REQUIRE(doc.at("protocol_version").is_number_integer());
    REQUIRE(doc.at("min_cli_protocol_version").is_number_integer());
    REQUIRE(doc.at("auth").at("device_login").is_boolean());
}

TEST_CASE("this CLI speaks the snapshot protocol version") {
    std::ifstream input(std::string(ASTRAL_PROTOCOL_DIR) + "/snapshots/v2/well-known.json");
    REQUIRE(input.good());

    const auto doc = nlohmann::json::parse(input);
    REQUIRE(astral::core::kProtocolVersion >= doc.at("min_cli_protocol_version").get<int>());
}

namespace {

// openapi.yaml 是 YAML，CLI 无 YAML 依赖——按冻结文本做「存在性」钉子：
// CLI 消费的 wire 面（路径/字段/错误码/模式串）必须逐字出现在快照里，
// 快照被替换或漂移时这里第一个红。读取为纯字符串做子串断言。
std::string snapshotFile(const char* name) {
    std::ifstream input(std::string(ASTRAL_PROTOCOL_DIR) + "/snapshots/v2/" + name);
    REQUIRE(input.good());
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

nlohmann::json snapshotJson(const char* name) {
    return nlohmann::json::parse(snapshotFile(name));
}

} // namespace

TEST_CASE("documents endpoints are pinned in the frozen snapshot") {
    const std::string openapi = snapshotFile("openapi.yaml");
    // 按值迭代 const char* 初始化列表：const std::string& 绑定 const char*
    // 临时量会触发 gcc 的 -Wrange-loop-construct（CI -Werror）。
    for (const char* const needle : {
             "/workspaces/{workspace_id}/documents/manifest:",
             "/workspaces/{workspace_id}/documents/{path}:",
             "/workspaces/{workspace_id}/conflicts:",
             "/workspaces/{workspace_id}/conflicts/{conflict_id}:",
             "/workspaces/{workspace_id}/conflicts/{conflict_id}/resolve:",
             // v2.3（round 41）：history / get --revision 消费的历史链端点。
             "/workspaces/{workspace_id}/document-versions:",
             "/workspaces/{workspace_id}/document-versions/{revision}:", "getDocumentManifest",
             "pushDocument", "deleteDocument", "resolveConflict", "listDocumentVersions",
             "getDocumentVersion",
             // 历史链 schema 名与 dvh 前缀（列表/详情形状 + ID 枚举）。
             "DocumentVersion:", "DocumentVersionDetail:", "/dvh/",
             // CLI 本地实现所依赖的语义标注（R1 大小写冲突与 R2 版本头的
             // 约定在 modulator docs/protocol.md，不在本快照文件集内——
             // 行为由单测覆盖，这里只钉 openapi 内的事实）。
             "document_sync",       // capabilities feature
             "sha256:[0-9a-f]{64}", // content_hash 形状
         }) {
        INFO("needle: " << needle);
        REQUIRE(openapi.find(needle) != std::string::npos);
    }
    // push 幂等与 tombstone 语义必须有契约标注（CLI 依赖它们设计 key 与复活路径）。
    REQUIRE(openapi.find("Idempotency-Key") != std::string::npos);
    REQUIRE(openapi.find("base_revision") != std::string::npos);
}

TEST_CASE("error enum covers the codes this CLI branches on") {
    const auto schema = snapshotJson("error.schema.json");
    const auto codes = schema.at("$defs").at("ErrorCode").at("enum");
    const auto has = [&codes](const char* code) {
        return std::find(codes.begin(), codes.end(), nlohmann::json(code)) != codes.end();
    };
    REQUIRE(has("DOCUMENT_CONFLICT"));
    REQUIRE(has("CLIENT_VERSION_UNSUPPORTED"));
    REQUIRE(has("RATE_LIMITED"));
    REQUIRE(has("INVITE_INVALID"));
    // round 38 起 NOT_IMPLEMENTED 已从契约移除（501 清零）——CLI 不得依赖。
    REQUIRE_FALSE(has("NOT_IMPLEMENTED"));
}

TEST_CASE("event enum covers document lifecycle events") {
    const auto schema = snapshotJson("event.schema.json");
    const std::string text = schema.dump();
    REQUIRE(text.find("document.updated") != std::string::npos);
    REQUIRE(text.find("document.conflict") != std::string::npos);
}
