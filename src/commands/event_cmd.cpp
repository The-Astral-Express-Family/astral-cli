#include "commands/event_cmd.hpp"

#include <memory>
#include <optional>
#include <string>

#include <CLI/CLI.hpp>

#include "auth/api.hpp"
#include "auth/session.hpp"
#include "client/http_client.hpp"
#include "commands/command.hpp"
#include "core/env.hpp"
#include "core/error.hpp"
#include "events/listen.hpp"
#include "platform/credential_store.hpp"

namespace astral::commands {

namespace {

using events::ListenOptions;

class EventCommand final : public Command {
public:
    const char* name() const override { return "event"; }
    const char* description() const override {
        return "Consume the workspace event stream (SSE -> JSON Lines)";
    }

    void configure(CLI::App& app) override {
        node_ = &app;
        app.require_subcommand(1);
        CLI::App* listen = app.add_subcommand(
            "listen", "Stream workspace events (reconnects and resumes automatically)");
        listen->add_option("--max-events", maxEvents_,
                           "Stop after this many events (default: run until interrupted)");
        listenSub_ = listen;
    }

    int execute(const CommandContext& context) override {
        if (node_->got_subcommand(listenSub_)) {
            return runListen(context);
        }
        throw core::AstralError(core::Errc::Usage, "no event subcommand selected");
    }

private:
    // Auth policy mirrors ApiSession::send (ARCHITECTURE.md section 6.3):
    // ASTRAL_TOKEN wins outright and never refreshes; otherwise the human
    // session slot rotates through /auth/token/refresh once per connection
    // attempt (a revoked family erases the session and fails with
    // AUTH_REQUIRED). Streaming responses never double-feed the parser: a 401
    // body is buffered by sendStreaming, so the refresh replay is the only
    // attempt that reaches the sink.
    events::AttemptFn makeAttempt(platform::CredentialStore& store,
                                  const auth::ServerInfo& server) const {
        client::HttpClient http;
        events::AttemptFn stream = [&http](const client::HttpRequest& request,
                                           const client::ChunkSink& sink) {
            return http.sendStreaming(request, sink);
        };
        if (auto envToken = core::env::get("ASTRAL_TOKEN"); envToken && !envToken->empty()) {
            return [envToken = *envToken, stream = std::move(stream)](
                       const client::HttpRequest& request, const client::ChunkSink& sink) {
                client::HttpRequest authed = request;
                authed.bearerToken = envToken;
                return stream(authed, sink);
            };
        }
        auto session = auth::requireSession(store, server.baseUrl);
        const auth::HttpFn transport = auth::realHttp();
        return [&store, &session, transport, stream = std::move(stream)](
                   const client::HttpRequest& request, const client::ChunkSink& sink) {
            return auth::withLazyRefresh(store, transport, session,
                                         [&](const platform::LoginSession& s) {
                                             client::HttpRequest authed = request;
                                             authed.bearerToken = s.accessToken;
                                             return stream(authed, sink);
                                         });
        };
    }

    int runListen(const CommandContext& context) {
        // One session per run: discovery exactly once; unresolvable targets
        // get the D14 default-workspace hint from openWorkspace.
        auto [api, ws] = auth::openWorkspace(context.server, context.workspace);
        const std::string url = auth::apiUrl(api, "/workspaces/" + ws.workspaceId + "/events");
        context.err << "listening to " << url << " (Ctrl+C to stop)\n";

        auto store = platform::makeDefaultCredentialStore();
        ListenOptions options;
        options.url = url;
        options.maxEvents = maxEvents_;
        return events::runListenLoop(options, makeAttempt(*store, api.server()), auth::realSleep,
                                     context.out, context.err, context.json);
    }

    CLI::App* node_ = nullptr;
    CLI::App* listenSub_ = nullptr;
    int maxEvents_ = 0;
};

} // namespace

std::unique_ptr<Command> makeEventCommand() {
    return std::make_unique<EventCommand>();
}

} // namespace astral::commands
