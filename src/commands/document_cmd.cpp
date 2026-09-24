// astral document 命令族（phase-5 第一层：协议端点的直接命令面）。
// 本层只做「读基准指针 -> 单文件读写 -> 冲突即失败并指向 conflicts」；
// 扫描目录/三方合并/批量 sync 引擎是下一层（FR-010），不在这里长出来。
//
// 语义对齐（快照 v2 MANIFEST key_semantics.documents / documents_delete /
// conflicts）：content_hash = sha256:<hex> 基于原始 UTF-8 bytes（不重写换行）；
// push 携带确定性 Idempotency-Key（重跑重放首次 2xx，避免伪冲突工件）；
// 409 DOCUMENT_CONFLICT 的 details.conflict_id 提升为可操作的提示。
#include "commands/document_cmd.hpp"

#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include "auth/api.hpp"
#include "commands/command.hpp"
#include "commands/paging.hpp"
#include "core/content_hash.hpp"
#include "core/error.hpp"
#include "output/json_output.hpp"
#include "output/render.hpp"
#include "platform/stdin.hpp"

namespace astral::commands {

namespace {

using nlohmann::json;
using output::printJson;
using output::printMoreHint;
using output::printPageJson;
using output::scalarOr;

constexpr std::size_t kMaxContentBytes = 1 << 20; // 服务端 maxContentBytes（1MiB）
constexpr std::size_t kMaxSegmentBytes = 255;
constexpr std::size_t kMaxPathBytes = 512;

// 结构校验（快路径 UX 错误；保留前缀黑名单/大小写冲突等权威判定留给
// 服务端 paths.go——客户端复刻只会漂移）。编码口径：按 '/' 分段、段内
// urlEncode、'/' 保持字面（openapi documents/{path} 的严格 URL 编码约定）。
std::string encodeDocumentPath(const std::string& path) {
    if (path.empty()) {
        throw core::AstralError(core::Errc::Usage, "document path is empty");
    }
    if (path.size() > kMaxPathBytes) {
        throw core::AstralError(core::Errc::Usage, "document path exceeds 512 bytes");
    }
    if (path.find('\\') != std::string::npos) {
        throw core::AstralError(core::Errc::Usage,
                                "document path uses backslashes; separate segments with '/'");
    }
    std::string encoded;
    std::size_t start = 0;
    while (true) {
        const std::size_t slash = path.find('/', start);
        const std::string segment =
            path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (segment.empty()) {
            throw core::AstralError(core::Errc::Usage,
                                    "document path has an empty segment (leading, trailing or "
                                    "double '/'): '" +
                                        path + "'");
        }
        if (segment == "." || segment == "..") {
            throw core::AstralError(core::Errc::Usage,
                                    "document path must not contain '.' or '..' segments");
        }
        if (segment.size() > kMaxSegmentBytes) {
            throw core::AstralError(core::Errc::Usage,
                                    "document path segment exceeds 255 bytes: '" + segment + "'");
        }
        for (const unsigned char byte : segment) {
            if (byte < 0x20 || byte == 0x7F) {
                throw core::AstralError(core::Errc::Usage,
                                        "document path contains control characters");
            }
        }
        if (!encoded.empty()) {
            encoded += '/';
        }
        encoded += client::urlEncode(segment);
        if (slash == std::string::npos) {
            return encoded;
        }
        start = slash + 1;
    }
}

// --file <path>|'-'（stdin）或 --body <text> 二选一；返回原始 bytes。
// 「是否显式给出」由调用方以 option count() 判定后传入：CLI11 对空串值不
// 置位 optional，而 --body "" 是合法的空文档，不得被当成「未提供」。
// 只做 UTF-8 与大小校验，不做任何规范化（hash 必须覆盖实际发送的 bytes）。
std::string loadContent(bool fileGiven, const std::string& file, bool bodyGiven,
                        const std::string& body) {
    std::string content;
    if (fileGiven && bodyGiven) {
        throw core::AstralError(core::Errc::Usage, "pass either --file or --body, not both");
    }
    if (fileGiven) {
        if (file == "-") {
            content = platform::readStdinBinary();
        } else {
            std::ifstream input(file, std::ios::binary);
            if (!input) {
                throw core::AstralError(core::Errc::Usage, "cannot open '" + file + "'");
            }
            // 分块 append 而非 istreambuf_iterator 的 assign：gcc13 对后者的
            // 库内联路径有 -Wnull-dereference 误报（CI arm -Werror 红源），
            // 块状读取在任何流上都等价且无此告警面。
            char buffer[8192];
            while (input.read(buffer, sizeof buffer) || input.gcount() > 0) {
                content.append(buffer, static_cast<std::size_t>(input.gcount()));
            }
        }
    } else if (bodyGiven) {
        content = body;
    } else {
        throw core::AstralError(core::Errc::Usage,
                                "content source required: --file <path>|- or --body <text>");
    }
    if (!core::isValidUtf8(content)) {
        throw core::AstralError(
            core::Errc::Usage,
            "content is not valid UTF-8 (documents are UTF-8 text; binary is out of scope)");
    }
    if (content.size() > kMaxContentBytes) {
        throw core::AstralError(core::Errc::Usage, "content exceeds the 1MiB document limit");
    }
    return content;
}

// push/delete 的乐观并发基准（协议：base_revision + base_hash）。
struct BasePointer {
    std::int64_t revision = 0;
    std::string hash;
};

// GET 文档；404（且非 workspace 级）返回 nullopt，其余非 2xx 走错误 envelope。
std::optional<json> fetchDocument(const auth::ApiSession& api, const std::string& wsId,
                                  const std::string& encodedPath) {
    client::HttpRequest request;
    request.url = auth::apiUrl(api, "/workspaces/" + wsId + "/documents/" + encodedPath);
    const client::HttpResponse response = api.send(std::move(request));
    if (response.status == 404) {
        try {
            const std::string code =
                json::parse(response.body).at("error").value("code", std::string());
            if (code != "WORKSPACE_NOT_FOUND") {
                return std::nullopt; // 文档（或 tombstone 之外的路径）不存在
            }
        } catch (const std::exception&) {
            return std::nullopt; // 无 envelope 的 404：按文档不存在处理
        }
    }
    if (response.status < 200 || response.status >= 300) {
        auth::throwApiError(response, "document fetch");
    }
    return json::parse(response.body);
}

// 基准指针解析：
//  - 显式 --base-revision 0：创建/复活，hash 用空内容占位（契约只要求形如
//    sha256:<hex>，base_revision=0 时服务端不比对）；
//  - 显式 --base-revision >0：必须同时给 --base-hash（显式 base = 信任本地
//    状态，半截指针无意义）；
//  - 缺省：GET 远端当前行（读改写，todo claim 同款节拍）——tombstone 行
//    归零为复活路径，404 归零为创建路径。
BasePointer resolveBasePointer(const auth::ApiSession& api, const std::string& wsId,
                               const std::string& encodedPath,
                               const std::optional<std::int64_t>& explicitRevision,
                               const std::string& explicitHash) {
    const std::string emptyHash = core::sha256ContentHash("");
    if (explicitRevision) {
        if (*explicitRevision == 0) {
            return BasePointer{0, explicitHash.empty() ? emptyHash : explicitHash};
        }
        if (explicitHash.empty()) {
            throw core::AstralError(core::Errc::Usage,
                                    "--base-revision > 0 requires --base-hash (the hash you "
                                    "last saw at that revision)");
        }
        return BasePointer{*explicitRevision, explicitHash};
    }
    const std::optional<json> doc = fetchDocument(api, wsId, encodedPath);
    if (!doc || doc->value("deleted", false)) {
        return BasePointer{0, emptyHash};
    }
    return BasePointer{doc->value("revision", std::int64_t{0}),
                       doc->value("content_hash", std::string())};
}

// 409 DOCUMENT_CONFLICT 的 details.conflict_id 不在错误 envelope 的稳定字段
// 里，throwApiError 会丢掉它——push/delete 落冲突时单独解析，给出可操作
// 的下一步（conflicts show/resolve）；其余 409 照旧走 throwApiError。
[[noreturn]] void throwConflictHint(const client::HttpResponse& response, const std::string& what) {
    try {
        const json envelope = json::parse(response.body).at("error");
        const std::string conflictId =
            envelope.value("details", json::object()).value("conflict_id", std::string());
        if (envelope.value("code", std::string()) == "DOCUMENT_CONFLICT" && !conflictId.empty()) {
            core::AstralError error{core::Errc::Conflict,
                                    what + ": " + envelope.value("message", std::string()) +
                                        " (conflict " + conflictId +
                                        "; inspect with `astral document conflicts show " +
                                        conflictId + "`)"};
            std::optional<std::string> requestId;
            if (auto it = envelope.find("request_id"); it != envelope.end() && it->is_string()) {
                requestId = it->get<std::string>();
            }
            std::optional<bool> retryable;
            if (auto it = envelope.find("retryable"); it != envelope.end() && it->is_boolean()) {
                retryable = it->get<bool>();
            }
            error.withProtocol("DOCUMENT_CONFLICT", std::move(requestId), std::move(retryable));
            throw error;
        }
    } catch (const core::AstralError&) {
        throw;
    } catch (const std::exception&) {
        // 无 envelope 或形状不符：退回通用错误映射。
    }
    auth::throwApiError(response, what);
}

class DocumentCommand final : public Command {
public:
    const char* name() const override { return "document"; }
    const char* description() const override { return "Sync workspace documents (phase-5)"; }

    void configure(CLI::App& app) override {
        node_ = &app;
        app.require_subcommand(1);

        CLI::App* manifest =
            app.add_subcommand("manifest", "List managed documents (path-ordered)");
        manifest->add_flag("--include-deleted", includeDeleted_,
                           "Include tombstone rows (deleted=true)");
        addPageFlags(*manifest, pageFlags_, "Page size (server max 1000)");

        CLI::App* get = app.add_subcommand("get", "Fetch one document");
        get->add_option("path", getPath_, "Repository-relative document path")->required();
        get->add_flag("--raw", getRaw_, "Print the content only (scripting mode)");
        // 路径是仓库相对的，不该被 CLI11 按 Windows 风格选项（/x）吞掉——
        // 关掉后 '/abs.md' 落进本地路径校验，给出准确错误而不是"缺参数"。
        get->allow_windows_style_options(false);

        CLI::App* push = app.add_subcommand(
            "push", "Push content with optimistic concurrency (Idempotency-Key safe)");
        push->allow_windows_style_options(false);
        push->add_option("path", pushPath_, "Repository-relative document path")->required();
        pushFileOpt_ =
            push->add_option("--file", pushFile_, "Read content from <path> ('-' = stdin)");
        pushBodyOpt_ = push->add_option("--body", pushBody_, "Inline content text");
        push->add_option("--base-revision", pushBaseRevision_,
                         "Base revision you last saw (omit: fetch current; 0: create/revive)");
        push->add_option("--base-hash", pushBaseHash_,
                         "sha256:<hex> content hash at the base revision");

        CLI::App* remove =
            app.add_subcommand("delete", "Tombstone-delete one document (versioned, never blind)");
        remove->allow_windows_style_options(false);
        remove->add_option("path", deletePath_, "Repository-relative document path")->required();
        remove->add_option("--base-revision", deleteBaseRevision_,
                           "Base revision you last saw (omit: fetch current)");

        CLI::App* conflicts = app.add_subcommand("conflicts", "Inspect and resolve conflicts");
        conflicts->require_subcommand(1);
        CLI::App* list = conflicts->add_subcommand("list", "List document conflicts");
        list->add_option("--status", conflictStatus_, "open | resolved | all")
            ->check(CLI::IsMember(std::set<std::string>{"open", "resolved", "all"}))
            ->default_val("open");
        addPageFlags(*list, conflictPageFlags_);
        CLI::App* show = conflicts->add_subcommand("show", "Show both sides of one conflict");
        show->add_option("conflict_id", conflictId_, "Conflict id (dfc_/cfg_...)")->required();
        CLI::App* resolve = conflicts->add_subcommand(
            "resolve", "Resolve a conflict (ours | theirs | merged | manual)");
        resolve->add_option("conflict_id", conflictId_, "Conflict id")->required();
        resolve->add_option("--resolution", resolution_, "ours | theirs | merged | manual")
            ->required()
            ->check(CLI::IsMember(std::set<std::string>{"ours", "theirs", "merged", "manual"}));
        resolveFileOpt_ = resolve->add_option(
            "--file", resolveFile_,
            "Final content from <path> ('-' = stdin); required for merged|manual");
        resolveBodyOpt_ = resolve->add_option("--body", resolveBody_, "Inline final content text");

        manifestSub_ = manifest;
        getSub_ = get;
        pushSub_ = push;
        deleteSub_ = remove;
        conflictsSub_ = conflicts;
        listSub_ = list;
        showSub_ = show;
        resolveSub_ = resolve;
    }

    int execute(const CommandContext& context) override {
        if (node_->got_subcommand(manifestSub_)) {
            return runManifest(context);
        }
        if (node_->got_subcommand(getSub_)) {
            return runGet(context);
        }
        if (node_->got_subcommand(pushSub_)) {
            return runPush(context);
        }
        if (node_->got_subcommand(deleteSub_)) {
            return runDelete(context);
        }
        if (node_->got_subcommand(conflictsSub_)) {
            if (conflictsSub_->got_subcommand(listSub_)) {
                return runConflictList(context);
            }
            if (conflictsSub_->got_subcommand(showSub_)) {
                return runConflictShow(context);
            }
            if (conflictsSub_->got_subcommand(resolveSub_)) {
                return runConflictResolve(context);
            }
        }
        throw core::AstralError(core::Errc::Usage, "no document subcommand selected");
    }

private:
    int runManifest(const CommandContext& context) {
        auto [api, ws] = auth::openWorkspace(context.server, context.workspace);
        std::string query;
        if (includeDeleted_) {
            query = "include_deleted=true";
        }
        addPageParams(query, pageFlags_);
        std::string nextCursor;
        const json items =
            auth::fetchPageItems(api, "/workspaces/" + ws.workspaceId + "/documents/manifest",
                                 std::move(query), pageFlags_.all, "document manifest", nextCursor);

        if (context.json) {
            printPageJson(context.out, ws.workspaceId, items, nextCursor);
            return 0;
        }
        if (items.empty()) {
            context.out << "No documents.\n";
            return 0;
        }
        for (const auto& item : items) {
            context.out << "r" << item.value("revision", std::int64_t{0}) << "  "
                        << item.value("size", std::int64_t{0}) << "B  " << scalarOr(item, "path");
            if (item.value("deleted", false)) {
                context.out << "  (deleted)";
            }
            context.out << '\n';
        }
        if (!nextCursor.empty()) {
            printMoreHint(context.out, items.size(), "--all");
        }
        return 0;
    }

    int runGet(const CommandContext& context) {
        auto [api, ws] = auth::openWorkspace(context.server, context.workspace);
        const std::string encoded = encodeDocumentPath(getPath_);
        const std::optional<json> doc = fetchDocument(api, ws.workspaceId, encoded);
        if (!doc) {
            throw core::AstralError(core::Errc::NotFound,
                                    "document '" + getPath_ + "' does not exist");
        }
        if (getRaw_) {
            context.out << doc->value("content", std::string());
            return 0;
        }
        if (context.json) {
            printJson(context.out, *doc);
            return 0;
        }
        context.out << getPath_ << "  revision " << doc->value("revision", std::int64_t{0}) << "  "
                    << scalarOr(doc, "content_hash") << "  " << scalarOr(doc, "updated_at");
        if (doc->value("deleted", false)) {
            context.out << "  (deleted)";
        }
        context.out << "\n\n" << doc->value("content", std::string());
        if (const std::string& content = doc->at("content").get_ref<const std::string&>();
            content.empty() || content.back() != '\n') {
            context.out << '\n';
        }
        return 0;
    }

    int runPush(const CommandContext& context) {
        auto [api, ws] = auth::openWorkspace(context.server, context.workspace);
        const std::string encoded = encodeDocumentPath(pushPath_);
        const std::string content =
            loadContent(pushFileOpt_->count() > 0, pushFile_, pushBodyOpt_->count() > 0, pushBody_);
        const std::string contentHash = core::sha256ContentHash(content);
        const BasePointer base =
            resolveBasePointer(api, ws.workspaceId, encoded, pushBaseRevision_, pushBaseHash_);

        json body{{"base_revision", base.revision},
                  {"base_hash", base.hash},
                  {"content", content},
                  {"content_hash", contentHash}};
        client::HttpRequest request;
        request.method = "PUT";
        request.url = auth::apiUrl(api, "/workspaces/" + ws.workspaceId + "/documents/" + encoded);
        request.body = body.dump();
        request.headers.emplace_back("Content-Type", "application/json");
        // 重跑同一命令重放首次 2xx：无此 key 时，成功后的重复 push 会以
        // 失配 base 命中 409 并在服务端落下伪冲突工件（副作用不可收回）。
        request.headers.emplace_back("Idempotency-Key",
                                     "doc-" + core::fnv1aHex(pushPath_ + "|" +
                                                             std::to_string(base.revision) + "|" +
                                                             base.hash + "|" + contentHash));
        const client::HttpResponse response = api.send(std::move(request));
        if (response.status == 409) {
            throwConflictHint(response, "document push");
        }
        if (response.status < 200 || response.status >= 300) {
            auth::throwApiError(response, "document push");
        }
        const json doc = json::parse(response.body);
        if (context.json) {
            printJson(context.out, doc);
            return 0;
        }
        context.out << "Pushed " << pushPath_ << " (revision "
                    << doc.value("revision", std::int64_t{0}) << ", " << contentHash.substr(0, 19)
                    << "…)\n";
        return 0;
    }

    int runDelete(const CommandContext& context) {
        auto [api, ws] = auth::openWorkspace(context.server, context.workspace);
        const std::string encoded = encodeDocumentPath(deletePath_);
        std::int64_t baseRevision = 0;
        if (deleteBaseRevision_) {
            baseRevision = *deleteBaseRevision_;
        } else {
            const std::optional<json> doc = fetchDocument(api, ws.workspaceId, encoded);
            if (!doc) {
                throw core::AstralError(core::Errc::NotFound,
                                        "document '" + deletePath_ + "' does not exist remotely");
            }
            baseRevision = doc->value("revision", std::int64_t{0});
        }

        client::HttpRequest request;
        request.method = "DELETE";
        request.url = auth::apiUrl(api, "/workspaces/" + ws.workspaceId + "/documents/" + encoded +
                                            "?base_revision=" + std::to_string(baseRevision));
        const client::HttpResponse response = api.send(std::move(request));
        if (response.status == 409) {
            throwConflictHint(response, "document delete");
        }
        if (response.status < 200 || response.status >= 300) {
            auth::throwApiError(response, "document delete");
        }
        if (context.json) {
            printJson(context.out, json{{"path", deletePath_}, {"deleted", true}});
            return 0;
        }
        context.out << "Deleted " << deletePath_ << " (tombstone at base revision " << baseRevision
                    << ")\n";
        return 0;
    }

    int runConflictList(const CommandContext& context) {
        auto [api, ws] = auth::openWorkspace(context.server, context.workspace);
        std::string query = "status=" + conflictStatus_;
        addPageParams(query, conflictPageFlags_);
        std::string nextCursor;
        const json items = auth::fetchPageItems(api, "/workspaces/" + ws.workspaceId + "/conflicts",
                                                std::move(query), conflictPageFlags_.all,
                                                "conflict list", nextCursor);
        if (context.json) {
            printPageJson(context.out, ws.workspaceId, items, nextCursor);
            return 0;
        }
        if (items.empty()) {
            context.out << "No conflicts.\n";
            return 0;
        }
        for (const auto& conflict : items) {
            context.out << scalarOr(conflict, "id") << "  " << scalarOr(conflict, "status") << "  "
                        << scalarOr(conflict, "path") << "  " << scalarOr(conflict, "created_at")
                        << '\n';
        }
        if (!nextCursor.empty()) {
            printMoreHint(context.out, items.size(), "--all");
        }
        return 0;
    }

    int runConflictShow(const CommandContext& context) {
        auto [api, ws] = auth::openWorkspace(context.server, context.workspace);
        const json detail = auth::getJson(
            api, "/workspaces/" + ws.workspaceId + "/conflicts/" + conflictId_, "conflict detail");
        if (context.json) {
            printJson(context.out, detail);
            return 0;
        }
        context.out << scalarOr(detail, "id") << "  " << scalarOr(detail, "path") << "  status "
                    << scalarOr(detail, "status") << "  base r"
                    << detail.value("base_revision", std::int64_t{0}) << " "
                    << scalarOr(detail, "base_hash") << '\n';
        const std::string resolution = scalarOr(detail, "resolution");
        if (!resolution.empty()) {
            context.out << "resolved: " << resolution << " by " << scalarOr(detail, "resolved_by")
                        << " at " << scalarOr(detail, "resolved_at") << '\n';
        }
        context.out << "\n--- ours (push, " << scalarOr(detail, "ours_hash") << ") ---\n"
                    << detail.value("ours_content", std::string()) << "\n--- theirs (server r"
                    << detail.value("theirs_revision", std::int64_t{0}) << ", "
                    << scalarOr(detail, "theirs_hash") << ") ---\n"
                    << detail.value("theirs_content", std::string()) << '\n';
        return 0;
    }

    int runConflictResolve(const CommandContext& context) {
        auto [api, ws] = auth::openWorkspace(context.server, context.workspace);
        const bool needsContent = resolution_ == "merged" || resolution_ == "manual";
        const bool fileGiven = resolveFileOpt_->count() > 0;
        const bool bodyGiven = resolveBodyOpt_->count() > 0;
        if (!needsContent && (fileGiven || bodyGiven)) {
            throw core::AstralError(core::Errc::Usage,
                                    "--file/--body only apply to --resolution merged|manual");
        }
        json body{{"resolution", resolution_}};
        if (needsContent) {
            body["content"] = loadContent(fileGiven, resolveFile_, bodyGiven, resolveBody_);
        }
        const json doc = auth::sendJson(
            api, "POST", "/workspaces/" + ws.workspaceId + "/conflicts/" + conflictId_ + "/resolve",
            body, "conflict resolve");
        if (context.json) {
            printJson(context.out, doc);
            return 0;
        }
        context.out << "Resolved " << conflictId_ << " (" << resolution_
                    << "): " << scalarOr(doc, "path") << " now at revision "
                    << doc.value("revision", std::int64_t{0}) << '\n';
        return 0;
    }

    CLI::App* node_ = nullptr;
    CLI::App* manifestSub_ = nullptr;
    CLI::App* getSub_ = nullptr;
    CLI::App* pushSub_ = nullptr;
    CLI::App* deleteSub_ = nullptr;
    CLI::App* conflictsSub_ = nullptr;
    CLI::App* listSub_ = nullptr;
    CLI::App* showSub_ = nullptr;
    CLI::App* resolveSub_ = nullptr;

    bool includeDeleted_ = false;
    PageFlags pageFlags_;
    std::string getPath_;
    bool getRaw_ = false;
    std::string pushPath_;
    std::string pushFile_;
    std::string pushBody_;
    CLI::Option* pushFileOpt_ = nullptr;
    CLI::Option* pushBodyOpt_ = nullptr;
    std::optional<std::int64_t> pushBaseRevision_;
    std::string pushBaseHash_;
    std::string deletePath_;
    std::optional<std::int64_t> deleteBaseRevision_;
    std::string conflictStatus_ = "open";
    PageFlags conflictPageFlags_;
    std::string conflictId_;
    std::string resolution_;
    std::string resolveFile_;
    std::string resolveBody_;
    CLI::Option* resolveFileOpt_ = nullptr;
    CLI::Option* resolveBodyOpt_ = nullptr;
};

} // namespace

std::unique_ptr<Command> makeDocumentCommand() {
    return std::make_unique<DocumentCommand>();
}

} // namespace astral::commands
