#include "commands/profile_cmd.hpp"

#include <filesystem>
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
#include "workspace/binding.hpp"

namespace astral::commands {

namespace {

using nlohmann::json;
using output::scalarOr;

// Renders the Me envelope (snapshot v2.1: {actor, email?, session?}) for both
// `profile show` and after `profile set`. --json passes the envelope through
// verbatim; human output skips unset (empty-string) bio/avatar.
void renderMe(const CommandContext& context, const auth::ServerInfo& server, const json& me) {
    if (context.json) {
        output::printJson(context.out, me);
        return;
    }
    const json& actor = me.at("actor");
    context.out << scalarOr(actor, "display_name") << " (" << scalarOr(actor, "id") << ", "
                << scalarOr(actor, "kind") << ") on " << server.serverId << " (" << server.baseUrl
                << ")\n";
    if (const auto email = me.value("email", std::string()); !email.empty()) {
        context.out << "email:  " << email << '\n';
    }
    if (const auto bio = scalarOr(actor, "bio"); !bio.empty()) {
        context.out << "bio:    " << bio << '\n';
    }
    if (const auto avatar = scalarOr(actor, "avatar_url"); !avatar.empty()) {
        context.out << "avatar: " << avatar << '\n';
    }
}

class ProfileCommand final : public Command {
public:
    const char* name() const override { return "profile"; }
    const char* description() const override { return "Show or update your own actor profile"; }

    void configure(CLI::App& app) override {
        node_ = &app;
        app.require_subcommand(1);

        CLI::App* show = app.add_subcommand("show", "Show your profile (GET /auth/me)");
        show->add_option("server_url", showServer_, "Server base URL (default: --server/ASTRAL_SERVER)");

        CLI::App* set = app.add_subcommand("set", "Update your profile (PATCH /auth/me, partial)");
        set->add_option("server_url", setServer_, "Server base URL (default: --server/ASTRAL_SERVER)");
        set->add_option("--display-name", displayName_, "New display name (1-200 chars)");
        set->add_option("--bio", bio_, "New bio; empty string clears it (max 500 chars)");
        set->add_option("--avatar-url", avatarUrl_, "New avatar http(s) URL; empty string clears it");

        showSub_ = show;
        setSub_ = set;
    }

    int execute(const CommandContext& context) override {
        if (node_->got_subcommand(showSub_)) {
            return runShow(context);
        }
        if (node_->got_subcommand(setSub_)) {
            return runSet(context);
        }
        throw core::AstralError(core::Errc::Usage, "no profile subcommand selected");
    }

private:
    // Server resolution (ARCHITECTURE.md section 10): positional > --server >
    // repo binding > ASTRAL_SERVER. The binding is a convenience fallback so
    // a bound repo needs no repeated URL; the profile itself still belongs to
    // the credential, not to the repo.
    auth::ApiSession openApi(const std::optional<std::string>& positional,
                             const CommandContext& context) const {
        if ((positional && !positional->empty()) || context.server) {
            return auth::ApiSession(auth::resolveServerUrl(positional, context.server));
        }
        if (const auto bindingDir = workspace::findBindingDir(std::filesystem::current_path())) {
            if (const auto binding = workspace::readBinding(*bindingDir)) {
                return auth::ApiSession(binding->serverUrl);
            }
        }
        return auth::ApiSession(auth::resolveServerUrl(std::nullopt, std::nullopt));
    }

    int runShow(const CommandContext& context) {
        auth::ApiSession api = openApi(showServer_, context);
        const json me = auth::getJson(api, "/auth/me", "profile show");
        renderMe(context, api.server(), me);
        return 0;
    }

    int runSet(const CommandContext& context) {
        if (!displayName_ && !bio_ && !avatarUrl_) {
            throw core::AstralError(core::Errc::Usage,
                                    "no profile field given (use --display-name, --bio, "
                                    "--avatar-url; repeat `profile show` to inspect)");
        }
        if (displayName_ && displayName_->empty()) {
            throw core::AstralError(core::Errc::Usage,
                                    "--display-name must not be empty (server rejects blank names)");
        }

        json body = json::object();
        if (displayName_) {
            body["display_name"] = *displayName_;
        }
        if (bio_) {
            body["bio"] = *bio_;
        }
        if (avatarUrl_) {
            body["avatar_url"] = *avatarUrl_;
        }

        auth::ApiSession api = openApi(setServer_, context);
        // Partial update: omitted fields stay untouched; bio/avatar accept the
        // empty string as "clear" (snapshot v2.1 actor_profile semantics).
        const json me = auth::sendJson(api, "PATCH", "/auth/me", body, "profile set");
        if (!context.json) {
            context.out << "Profile updated.\n";
        }
        renderMe(context, api.server(), me);
        return 0;
    }


    CLI::App* node_ = nullptr;
    CLI::App* showSub_ = nullptr;
    CLI::App* setSub_ = nullptr;

    std::string showServer_;
    std::string setServer_;
    std::optional<std::string> displayName_;
    std::optional<std::string> bio_;
    std::optional<std::string> avatarUrl_;
};

} // namespace

std::unique_ptr<Command> makeProfileCommand() {
    return std::make_unique<ProfileCommand>();
}

} // namespace astral::commands
