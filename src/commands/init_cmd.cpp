#include <memory>
#include <optional>
#include <string>

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include "auth/session.hpp"
#include "commands/command.hpp"
#include "commands/init_cmd.hpp"
#include "core/env.hpp"
#include "core/error.hpp"
#include "output/json_output.hpp"
#include "platform/credential_store.hpp"
#include "workspace/binding.hpp"
#include "workspace/target.hpp"

namespace astral::commands {

namespace {

// `astral init <server_url>[/<workspace_name>] [path]` follows the state
// machine in ARCHITECTURE.md section 9.3: parse the shorthand, discover the
// server, resolve (or --create) the workspace, verify membership visibility,
// and write `.astral/config.json` atomically.
class InitCommand final : public Command {
public:
    const char* name() const override { return "init"; }
    const char* description() const override { return "Bind a directory to an Astral workspace"; }

    void configure(CLI::App& app) override {
        app.add_option("server_url", serverRef_, "Server URL, optionally with /<workspace_name>")
            ->required();
        app.add_option("path", path_, "Directory to bind (default: current directory)");
        app.add_option("--workspace,-w", workspace_,
                       "Explicit workspace name (disables shorthand splitting)");
        app.add_flag("--create", create_, "Create the workspace if missing (non-interactive)");
        app.add_flag("--rebind", rebind_, "Replace an existing different binding");
    }

    int execute(const CommandContext& context) override {
        const auto explicitWorkspace = workspace_.empty() ? std::optional<std::string>{}
                                                          : std::optional<std::string>{workspace_};
        const workspace::TargetSpec spec =
            workspace::parseTargetSpec(serverRef_, explicitWorkspace);
        if (!spec.workspaceName) {
            throw core::AstralError(core::Errc::LocalWorkspaceError,
                                    "no workspace given: use <server>/<workspace>, --workspace, "
                                    "or a URL ending in the workspace name");
        }

        // 1. Discovery + session (init requires an authenticated human).
        const auth::ServerInfo server = auth::discoverServer(spec.fullUrl, auth::realHttp());
        auto store = platform::makeDefaultCredentialStore();
        platform::LoginSession session = auth::requireSession(*store, server.baseUrl);
        const std::string api = server.origin() + server.apiBase;

        // 2. Resolve the workspace by exact name; every authenticated call
        // goes through the lazy-refresh wrapper (D13: access tokens live
        // 5-15 minutes, rotation is transparent to this flow).
        const std::string workspaceName = *spec.workspaceName;
        const auth::HttpFn http = auth::realHttp();
        // Every authenticated call goes through the lazy-refresh wrapper
        // (D13: access tokens live 5-15 minutes; rotation is transparent).
        const auto authedGet = [&](const std::string& url) -> client::HttpResponse {
            const auto call = [&http, &url](const platform::LoginSession& s) {
                client::HttpRequest request;
                request.url = url;
                request.bearerToken = s.accessToken;
                return http(request);
            };
            return auth::withLazyRefresh(*store, http, session, call);
        };
        const auto authedPost = [&](const std::string& url,
                                    const std::string& body) -> client::HttpResponse {
            const auto call = [&http, &url, &body](const platform::LoginSession& s) {
                client::HttpRequest request;
                request.method = "POST";
                request.url = url;
                request.bearerToken = s.accessToken;
                request.body = body;
                return http(request);
            };
            return auth::withLazyRefresh(*store, http, session, call);
        };

        const client::HttpResponse lookup = authedGet(api + "/workspaces?name=" + workspaceName);
        if (lookup.status != 200) {
            throw core::AstralError(core::Errc::ProtocolIncompatible,
                                    "workspace lookup returned status " +
                                        std::to_string(lookup.status));
        }
        const nlohmann::json items =
            nlohmann::json::parse(lookup.body).value("items", nlohmann::json::array());
        nlohmann::json workspace;
        if (!items.empty()) {
            workspace = items.front();
        } else if (!create_) {
            throw core::AstralError(core::Errc::WorkspaceNotFound,
                                    "workspace '" + workspaceName +
                                        "' not found on the server (pass --create to make it)");
        } else {
            const client::HttpResponse created =
                authedPost(api + "/workspaces", nlohmann::json{{"name", workspaceName}}.dump());
            if (created.status == 409) {
                throw core::AstralError(core::Errc::WorkspaceAlreadyBound,
                                        "workspace name '" + workspaceName + "' is already taken");
            }
            if (created.status != 201) {
                throw core::AstralError(core::Errc::ProtocolIncompatible,
                                        "workspace creation returned status " +
                                            std::to_string(created.status));
            }
            workspace = nlohmann::json::parse(created.body).at("workspace");
        }

        // 3. Verify the binding target is visible to this principal (§10.9:
        // GET /workspaces/{id}; non-member -> 404).
        const std::string workspaceUrl =
            api + "/workspaces/" + workspace.value("id", std::string());
        const client::HttpResponse verified = authedGet(workspaceUrl);
        if (verified.status == 404) {
            throw core::AstralError(core::Errc::WorkspaceNotFound,
                                    "workspace '" + workspace.value("name", *spec.workspaceName) +
                                        "' is not visible to this principal (not a member?)");
        }
        if (verified.status != 200) {
            throw core::AstralError(core::Errc::ProtocolIncompatible,
                                    "workspace verification returned status " +
                                        std::to_string(verified.status));
        }
        workspace = nlohmann::json::parse(verified.body).at("workspace");

        // 4. Respect an existing binding; replace only with --rebind.
        const std::string dir = path_.empty() ? "." : path_;
        const workspace::Binding fresh{1,
                                       server.serverId,
                                       server.baseUrl,
                                       workspace.value("id", std::string()),
                                       workspace.value("name", std::string()),
                                       workspace.value("slug", std::string())};
        const auto existing = workspace::readBinding(dir);
        const bool alreadyBound = existing && workspace::sameTarget(*existing, fresh);
        if (existing && !alreadyBound && !rebind_) {
            throw core::AstralError(core::Errc::WorkspaceAlreadyBound,
                                    dir + " is bound to a different workspace (workspace " +
                                        existing->workspaceId + "); pass --rebind to replace it");
        }
        const bool rebound = existing && !alreadyBound;
        workspace::writeBinding(dir, fresh);

        if (context.json) {
            output::printJson(context.out, {{"bound", !alreadyBound},
                                            {"rebound", rebound},
                                            {"path", dir},
                                            {"server_id", fresh.serverId},
                                            {"server_url", fresh.serverUrl},
                                            {"workspace_id", fresh.workspaceId},
                                            {"workspace_name", fresh.workspaceName}});
        } else if (alreadyBound) {
            context.out << "Already bound to the same workspace; nothing to do\n";
        } else {
            context.out << "Bound " << fresh.workspaceName << " -> " << fresh.serverUrl << " ("
                        << fresh.workspaceId << ")\n";
        }
        return 0;
    }

private:
    std::string serverRef_;
    std::string path_;
    std::string workspace_;
    bool create_ = false;
    bool rebind_ = false;
};

} // namespace

std::unique_ptr<Command> makeInitCommand() {
    return std::make_unique<InitCommand>();
}

} // namespace astral::commands
