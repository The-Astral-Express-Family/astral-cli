#include <memory>
#include <optional>
#include <string>

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include "auth/device_flow.hpp"
#include "auth/session.hpp"
#include "commands/command.hpp"
#include "commands/login_cmd.hpp"
#include "core/error.hpp"
#include "output/json_output.hpp"
#include "platform/browser.hpp"
#include "platform/credential_store.hpp"

namespace astral::commands {

namespace {

// Positional > --server flag > ASTRAL_SERVER (ARCHITECTURE.md section 10).
std::string serverArg(const CommandContext& context, const std::optional<std::string>& positional) {
    return auth::resolveServerUrl(positional, context.server);
}

// Browser is best-effort; the manual URL/code path always goes to stderr so
// --json keeps stdout as a single object.
void presentUserCode(const std::string& url, const std::string& code, std::ostream& err) {
    const bool opened = platform::openInBrowser(url);
    err << (opened ? "browser opened for approval" : "browser unavailable") << "\n"
        << "approve at: " << url << "\n"
        << "user code:  " << code << "\n";
}

class LoginCommand final : public Command {
public:
    const char* name() const override { return "login"; }
    const char* description() const override { return "Log in to an Astral server (device flow)"; }

    void configure(CLI::App& app) override {
        app.add_option("server_url", serverUrl_, "Server base URL, e.g. https://astral.example.com")
            ->required();
    }

    int execute(const CommandContext& context) override {
        const std::string baseUrl = serverArg(context, serverUrl_);
        auto store = platform::makeDefaultCredentialStore();
        const platform::LoginSession session =
            auth::runDeviceFlow(baseUrl, auth::realHttp(), auth::realSleep,
                                [&](const std::string& url, const std::string& code) {
                                    presentUserCode(url, code, context.err);
                                });
        store->saveSession(session.serverUrl, session);

        if (context.json) {
            output::printJson(context.out, {{"server_url", session.serverUrl},
                                            {"server_id", session.serverId},
                                            {"principal_id", session.principalId}});
            return 0;
        }
        context.out << "Logged in as "
                    << (session.principalId.empty() ? "<unknown>" : session.principalId) << " on "
                    << session.serverId << " (" << session.serverUrl << ")\n";
        return 0;
    }

private:
    std::string serverUrl_;
};

class LogoutCommand final : public Command {
public:
    const char* name() const override { return "logout"; }
    const char* description() const override { return "Forget credentials for a server"; }

    void configure(CLI::App& app) override {
        app.add_option("server_url", serverUrl_,
                       "Server base URL (default: --server/ASTRAL_SERVER)");
    }

    int execute(const CommandContext& context) override {
        const std::string baseUrl = serverArg(context, serverUrl_);
        auto store = platform::makeDefaultCredentialStore();
        const auto session = store->loadSession(baseUrl);
        if (!session) {
            if (context.json) {
                output::printJson(context.out, {{"logged_out", false}, {"server_url", baseUrl}});
            } else {
                context.out << "Not logged in to " << baseUrl << "\n";
            }
            return 0;
        }

        // Revoke server-side, then drop the local pair regardless (the local
        // session is worthless once logout intent is expressed).
        try {
            const auth::HttpFn http = auth::realHttp();
            client::HttpRequest request;
            request.method = "POST";
            request.url = auth::ServerInfo{session->serverUrl, "", session->apiBase}.origin() +
                          session->apiBase + "/auth/logout";
            request.body = "{\"refresh_token\":\"" + session->refreshToken + "\"}";
            (void)http(request);
        } catch (const core::AstralError& error) {
            context.err << "warning: server-side logout failed (" << error.what()
                        << "); local session removed anyway\n";
        }
        store->eraseSession(baseUrl);

        if (context.json) {
            output::printJson(context.out, {{"logged_out", true}, {"server_url", baseUrl}});
        } else {
            context.out << "Logged out of " << baseUrl << "\n";
        }
        return 0;
    }

private:
    std::string serverUrl_;
};

class WhoamiCommand final : public Command {
public:
    const char* name() const override { return "whoami"; }
    const char* description() const override {
        return "Show the authenticated principal for a server";
    }

    void configure(CLI::App& app) override {
        app.add_option("server_url", serverUrl_,
                       "Server base URL (default: --server/ASTRAL_SERVER)");
    }

    int execute(const CommandContext& context) override {
        const std::string baseUrl = serverArg(context, serverUrl_);
        auto store = platform::makeDefaultCredentialStore();
        platform::LoginSession session = auth::requireSession(*store, baseUrl);

        const auth::ServerInfo server = auth::discoverServer(baseUrl, auth::realHttp());
        const auth::HttpFn http = auth::realHttp();
        const std::string meUrl = server.origin() + server.apiBase + "/auth/me";
        const auto call = [&http, &meUrl](const platform::LoginSession& s) {
            client::HttpRequest request;
            request.url = meUrl;
            request.bearerToken = s.accessToken;
            return http(request);
        };
        const client::HttpResponse response = auth::withLazyRefresh(*store, http, session, call);
        if (response.status == 404) {
            throw core::AstralError(core::Errc::ServerNotFound,
                                    "no astral API at " + server.origin() + server.apiBase);
        }
        if (response.status != 200) {
            throw core::AstralError(core::Errc::ProtocolIncompatible,
                                    "/auth/me returned status " + std::to_string(response.status));
        }
        const nlohmann::json body = nlohmann::json::parse(response.body);
        const nlohmann::json& actor = body.at("actor");

        if (context.json) {
            output::printJson(context.out, {{"server_url", session.serverUrl},
                                            {"server_id", session.serverId},
                                            {"actor", actor}});
            return 0;
        }
        context.out << actor.value("display_name", "<unknown>") << " (" << session.principalId
                    << ", " << actor.value("kind", "unknown") << ") on " << session.serverUrl
                    << "\n";
        return 0;
    }

private:
    std::string serverUrl_;
};

} // namespace

std::unique_ptr<Command> makeLoginCommand() {
    return std::make_unique<LoginCommand>();
}
std::unique_ptr<Command> makeLogoutCommand() {
    return std::make_unique<LogoutCommand>();
}
std::unique_ptr<Command> makeWhoamiCommand() {
    return std::make_unique<WhoamiCommand>();
}

} // namespace astral::commands
