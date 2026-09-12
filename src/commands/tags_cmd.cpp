#include "commands/tags_cmd.hpp"

#include <memory>
#include <optional>
#include <string>

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include "auth/api.hpp"
#include "commands/command.hpp"
#include "core/error.hpp"
#include "output/json_output.hpp"
#include "output/render.hpp"
#include "output/style.hpp"

namespace astral::commands {

namespace {

using nlohmann::json;
using output::Painter;
using output::printPageJson;
using output::scalarOr;

// Resolves a `<name-or-id>` argument to (id, canonical name). `tag_`-prefixed
// arguments are taken as ids verbatim; otherwise the workspace tag dictionary
// is matched case-insensitively by name (the server normalizes with
// NFKC+lowercase, which covers the plain-ASCII CLI cases).
struct TagRef {
    std::string id;
    std::string name;
};

TagRef resolveTag(const auth::ApiSession& api, const auth::WorkspaceContext& ws,
                  const std::string& nameOrId) {
    if (nameOrId.rfind("tag_", 0) == 0) {
        return TagRef{nameOrId, ""};
    }
    client::HttpRequest request;
    request.url = auth::apiUrl(api, "/workspaces/" + ws.workspaceId + "/tags");
    const json page = json::parse(api.requireSuccess(std::move(request), "tag list").body);
    std::string lower;
    lower.reserve(nameOrId.size());
    for (char c : nameOrId) {
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (const auto it = page.find("items"); it != page.end() && it->is_array()) {
        for (const auto& tag : *it) {
            const std::string name = scalarOr(tag, "name");
            std::string candidate;
            candidate.reserve(name.size());
            for (char c : name) {
                candidate += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (candidate == lower) {
                return TagRef{scalarOr(tag, "id"), name};
            }
        }
    }
    throw core::AstralError(core::Errc::NotFound,
                            "tag '" + nameOrId + "' not found in this workspace");
}

// First step of the two-step flow; returns the frozen TagProposal shape
// (proposal_id, confirm_code, expires_at, existing_tags).
json propose(const auth::ApiSession& api, const auth::WorkspaceContext& ws,
             const std::string& action, const std::string& name, const std::string& targetTagId) {
    json body{{"action", action}, {"name", name}};
    if (!targetTagId.empty()) {
        body["target_tag_id"] = targetTagId;
    }
    return auth::sendJson(api, "POST", "/workspaces/" + ws.workspaceId + "/tag-proposals", body,
                          "tag proposal");
}

// Second step: code + name are bound to the proposal (and its actor) server-side.
json confirm(const auth::ApiSession& api, const std::string& proposalId, const std::string& code,
             const std::string& name) {
    return auth::sendJson(api, "POST", "/tag-proposals/" + proposalId + "/confirm",
                          json{{"confirm_code", code}, {"name", name}}, "tag confirm");
}

void printProposalHints(std::ostream& out, const std::string& verb, const json& proposal) {
    out << "Proposal " << scalarOr(proposal, "proposal_id") << " created (expires "
        << scalarOr(proposal, "expires_at") << ")\n";
    out << "  review the " << proposal.value("existing_tags", json::array()).size()
        << " existing tag(s) above-named conflict risk before confirming\n";
    out << "Confirm with:\n  astral tags " << verb << " --proposal "
        << scalarOr(proposal, "proposal_id") << " --confirm " << scalarOr(proposal, "confirm_code")
        << "\n";
}

class TagsCommand final : public Command {
public:
    const char* name() const override { return "tags"; }
    const char* description() const override {
        return "Manage tags (two-step proposal/confirm, spelling pinned to --confirm)";
    }

    void configure(CLI::App& app) override {
        node_ = &app;
        app.require_subcommand(1);

        CLI::App* list = app.add_subcommand("list", "List the workspace tag dictionary");

        CLI::App* create = app.add_subcommand("create", "Propose (then confirm) a new tag");
        create->add_option("name", name_, "Tag name")->required();
        create->add_option("--proposal", proposalId_, "Proposal id from the propose step");
        create->add_option("--confirm", confirmCode_, "Confirm code from the propose step");

        CLI::App* rename = app.add_subcommand("rename", "Propose (then confirm) a tag rename");
        rename->add_option("target", target_, "Tag name or tag_ id")->required();
        rename->add_option("new_name", newName_, "New tag name")->required();
        rename->add_option("--proposal", proposalId_, "Proposal id from the propose step");
        rename->add_option("--confirm", confirmCode_, "Confirm code from the propose step");

        CLI::App* del = app.add_subcommand("delete", "Propose (then confirm) a tag deletion");
        del->add_option("target", target_, "Tag name or tag_ id")->required();
        del->add_option("--proposal", proposalId_, "Proposal id from the propose step");
        del->add_option("--confirm", confirmCode_, "Confirm code from the propose step");

        listSub_ = list;
        createSub_ = create;
        renameSub_ = rename;
        deleteSub_ = del;
    }

    int execute(const CommandContext& context) override {
        if (node_->got_subcommand(listSub_)) {
            return runList(context);
        }
        if (node_->got_subcommand(createSub_)) {
            return runMutate(context, Action::Create, "");
        }
        if (node_->got_subcommand(renameSub_)) {
            if (newName_.empty()) {
                throw core::AstralError(core::Errc::Usage, "tags rename needs a <new-name>");
            }
            return runMutate(context, Action::Rename, newName_);
        }
        if (node_->got_subcommand(deleteSub_)) {
            return runMutate(context, Action::Delete, "");
        }
        throw core::AstralError(core::Errc::Usage, "no tags subcommand selected");
    }

private:
    int runList(const CommandContext& context) {
        auto [api, ws] = auth::openWorkspace(context.server, context.workspace);

        std::string nextCursor;
        const json items = auth::fetchPageItems(api, "/workspaces/" + ws.workspaceId + "/tags", "",
                                                false, "tag list", nextCursor);
        if (context.json) {
            printPageJson(context.out, ws.workspaceId, items, nextCursor);
            return 0;
        }
        if (items.empty()) {
            context.out << "No tags.\n";
            return 0;
        }
        const Painter paint(context.color);
        for (const auto& tag : items) {
            context.out << padId(tag) << "  " << paint.key(scalarOr(tag, "name")) << '\n';
        }
        // 注：服务端 tag 词典目前不分页（next_cursor 恒空）；若服务端引入分页，
        // 此处需同步加 --limit/--all 与截断尾注（对齐 todo/msg list）。
        return 0;
    }

    // create/rename/delete share one flow: resolve target (rename/delete),
    // then either propose (printing the confirm hint) or confirm directly.
    enum class Action { Create, Rename, Delete };

    static const char* wireName(Action action) {
        switch (action) {
        case Action::Create: return "create";
        case Action::Rename: return "rename";
        case Action::Delete: return "delete";
        }
        return "create"; // unreachable; pacifies compilers
    }

    int runMutate(const CommandContext& context, Action action, const std::string& confirmName) {
        auto [api, ws] = auth::openWorkspace(context.server, context.workspace);

        std::string name = confirmName;
        std::string targetId;
        if (action != Action::Create) {
            const TagRef target = resolveTag(api, ws, target_);
            targetId = target.id;
            if (action == Action::Delete) {
                name = target.name; // confirm's name must match the proposal input
            }
        }
        if (name.empty()) {
            name = name_;
        }

        // The hint must be a complete, copy-pasteable command line (positional
        // arguments included) or the two-step flow dead-ends for humans.
        std::string verb;
        switch (action) {
        case Action::Create:
            verb = "create " + name_;
            break;
        case Action::Rename:
            verb = "rename " + target_ + " " + newName_;
            break;
        case Action::Delete:
            verb = "delete " + target_;
            break;
        }

        if (confirmCode_.empty() && proposalId_.empty()) {
            const json proposal = propose(api, ws, wireName(action), name, targetId);
            if (context.json) {
                output::printJson(context.out, proposal); // TagProposal verbatim
                return 0;
            }
            printProposalHints(context.out, verb, proposal);
            return 0;
        }
        if (confirmCode_.empty() || proposalId_.empty()) {
            throw core::AstralError(core::Errc::Usage,
                                    "--proposal and --confirm must be given together");
        }

        const json tag = confirm(api, proposalId_, confirmCode_, name);
        if (context.json) {
            output::printJson(context.out, tag); // Tag verbatim
            return 0;
        }
        std::string outcome;
        switch (action) {
        case Action::Create:
            outcome = "created";
            break;
        case Action::Rename:
            outcome = "renamed to " + name;
            break;
        case Action::Delete:
            outcome = "deleted";
            break;
        }
        context.out << "Tag " << scalarOr(tag, "id") << " " << outcome << '\n';
        return 0;
    }

    static std::string padId(const json& tag) { return scalarOr(tag, "id"); }

    CLI::App* node_ = nullptr;
    CLI::App* listSub_ = nullptr;
    CLI::App* createSub_ = nullptr;
    CLI::App* renameSub_ = nullptr;
    CLI::App* deleteSub_ = nullptr;

    std::string name_;
    std::string target_;
    std::string newName_;
    std::string proposalId_;
    std::string confirmCode_;
};

} // namespace

std::unique_ptr<Command> makeTagsCommand() {
    return std::make_unique<TagsCommand>();
}

} // namespace astral::commands
