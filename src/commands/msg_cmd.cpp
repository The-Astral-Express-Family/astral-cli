#include "commands/msg_cmd.hpp"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include "auth/api.hpp"
#include "commands/command.hpp"
#include "core/error.hpp"
#include "output/json_output.hpp"
#include "output/style.hpp"

namespace astral::commands {

namespace {

using nlohmann::json;
using output::Painter;

std::string scalarOr(const json& object, const char* key, const std::string& fallback = "") {
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) {
        return fallback;
    }
    if (it->is_string()) {
        return it->get<std::string>();
    }
    return it->dump();
}

// Cuts at a codepoint boundary so CJK bodies never get mojibake'd.
std::string truncateUtf8(const std::string& text, std::size_t maxCodepoints) {
    std::size_t codepoints = 0;
    std::size_t offset = 0;
    while (offset < text.size()) {
        if (codepoints == maxCodepoints) {
            return text.substr(0, offset) + "...";
        }
        const unsigned char lead = static_cast<unsigned char>(text[offset]);
        std::size_t len = 1;
        if ((lead & 0xE0) == 0xC0) {
            len = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            len = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            len = 4;
        }
        offset += len;
        ++codepoints;
    }
    return text;
}

// Target grammar (ARCHITECTURE.md section 11): `workspace` broadcasts to the
// bound workspace; `actor:<actor_id>` is a direct message; `task:<task_id>`
// posts into the task thread.
struct MsgTarget {
    std::string type;
    std::string id;
};

MsgTarget parseTarget(const std::string& spec, const std::string& workspaceId) {
    if (spec == "workspace") {
        return MsgTarget{"workspace", workspaceId};
    }
    const auto colon = spec.find(':');
    if (colon == std::string::npos) {
        throw core::AstralError(core::Errc::Usage,
                                "invalid target '" + spec +
                                    "' (use: workspace | actor:<actor_id> | task:<task_id>)");
    }
    const std::string type = spec.substr(0, colon);
    const std::string id = spec.substr(colon + 1);
    if (id.empty()) {
        throw core::AstralError(core::Errc::Usage,
                                "target '" + spec + "' is missing the id after ':'");
    }
    if (type == "actor" || type == "task") {
        return MsgTarget{type, id};
    }
    throw core::AstralError(core::Errc::Usage,
                            "invalid target type '" + type +
                                "' (use: workspace | actor:<actor_id> | task:<task_id>)");
}

// Deterministic content hash for the Idempotency-Key: re-running the same
// send must dedupe server-side (24h replay window), so the key must be stable
// across processes and platforms (std::hash gives neither guarantee).
std::string fnv1aHex(const std::string& text) {
    std::uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
    return buffer;
}

class MsgCommand final : public Command {
public:
    const char* name() const override { return "msg"; }
    const char* description() const override { return "Read and send workspace messages"; }

    void configure(CLI::App& app) override {
        node_ = &app;
        app.require_subcommand(1);

        CLI::App* send = app.add_subcommand("send", "Send a message (Idempotency-Key safe)");
        send->add_option("target", target_, "workspace | actor:<actor_id> | task:<task_id>")
            ->required();
        send->add_option("body", body_, "Message body")->required();
        send->add_option("--thread", thread_, "Thread to reply into (message id)");

        CLI::App* list = app.add_subcommand("list", "List messages");
        list->add_option("--task", taskFilter_, "List the thread of this task");
        list->add_option("--thread", threadFilter_, "List this thread only");
        list->add_option("--limit", limit_, "Page size (server max 200)");
        list->add_flag("--all", all_, "Follow next_cursor until exhausted");

        sendSub_ = send;
        listSub_ = list;
    }

    int execute(const CommandContext& context) override {
        if (node_->got_subcommand(sendSub_)) {
            return runSend(context);
        }
        if (node_->got_subcommand(listSub_)) {
            return runList(context);
        }
        throw core::AstralError(core::Errc::Usage, "no msg subcommand selected");
    }

private:
    int runSend(const CommandContext& context) {
        auto [api, ws] = auth::openWorkspace(context.server, context.workspace);
        const MsgTarget target = parseTarget(target_, ws.workspaceId);

        json body{{"target", {{"type", target.type}, {"id", target.id}}}, {"body", body_}};
        if (!thread_.empty()) {
            body["thread_id"] = thread_;
        }

        client::HttpRequest request;
        request.method = "POST";
        request.url = auth::apiUrl(api, "/workspaces/" + ws.workspaceId + "/messages");
        request.body = body.dump();
        request.headers.emplace_back("Content-Type", "application/json");
        // 重试不得双发：send 携带确定性 Idempotency-Key（同 actor+endpoint+key
        // 24h 内服务端重放首次 2xx），内容相同 → key 相同。
        request.headers.emplace_back(
            "Idempotency-Key",
            "msg-" + fnv1aHex(target.type + "|" + target.id + "|" + thread_ + "|" + body_));
        const json message =
            json::parse(api.requireSuccess(std::move(request), "message send").body);

        if (context.json) {
            output::printJson(context.out, message); // Message verbatim
            return 0;
        }
        context.out << "Sent " << scalarOr(message, "id") << " to " << target.type << " "
                    << target.id << '\n';
        return 0;
    }

    int runList(const CommandContext& context) {
        auto [api, ws] = auth::openWorkspace(context.server, context.workspace);

        // Container-specific endpoints (v2): --task uses the task thread
        // collection; otherwise the workspace collection with optional
        // thread filter.
        std::string path;
        std::string query;
        auto append = [&query](const std::string& key, const std::string& value) {
            if (value.empty()) {
                return;
            }
            if (!query.empty()) {
                query += '&';
            }
            query += key + '=' + client::urlEncode(value);
        };
        if (!taskFilter_.empty()) {
            path = "/tasks/" + taskFilter_ + "/messages";
        } else {
            path = "/workspaces/" + ws.workspaceId + "/messages";
            append("thread_id", threadFilter_);
        }
        if (limit_) {
            append("limit", std::to_string(*limit_));
        }
        std::string nextCursor;
        const json items =
            auth::fetchPageItems(api, path, std::move(query), all_, "message list", nextCursor);

        if (context.json) {
            output::printJson(
                context.out,
                {{"workspace_id", ws.workspaceId},
                 {"items", items},
                 {"next_cursor", nextCursor.empty() ? json(nullptr) : json(nextCursor)}});
            return 0;
        }
        if (items.empty()) {
            context.out << "No messages.\n";
            return 0;
        }
        for (const auto& message : items) {
            context.out << scalarOr(message, "id") << "  " << scalarOr(message, "sender_id") << "  "
                        << scalarOr(message, "created_at") << "  "
                        << truncateUtf8(scalarOr(message, "body"), 64) << '\n';
        }
        if (!nextCursor.empty()) {
            context.out << "(" << items.size() << " shown; more available - pass --all)\n";
        }
        return 0;
    }

    CLI::App* node_ = nullptr;
    CLI::App* sendSub_ = nullptr;
    CLI::App* listSub_ = nullptr;

    std::string target_;
    std::string body_;
    std::string thread_;
    std::string taskFilter_;
    std::string threadFilter_;
    std::optional<int> limit_;
    bool all_ = false;
};

} // namespace

std::unique_ptr<Command> makeMsgCommand() {
    return std::make_unique<MsgCommand>();
}

} // namespace astral::commands
