#include "commands/login_cmd.hpp"

#include <functional>
#include <string>
#include <utility>

#include "CLI/CLI.hpp"
#include "commands/command.hpp"
#include "core/error.hpp"

namespace astral::commands {

namespace {

// Login/logout/whoami need device-flow authorization against a live server
// (ARCHITECTURE.md section 6). The v0.1 scaffold wires the command surface
// and refuses cleanly instead of half-implementing an auth flow. The three
// share this stub skeleton — only name/description/optionality and the
// refusal message differ, keeping the "not implemented" semantics and the
// wording in one place.
class AuthStubCommand final : public Command {
public:
    using MessageFactory =
        std::function<std::string(const CommandContext&, const std::string& serverUrl)>;

    AuthStubCommand(const char* name, const char* description, bool serverRequired,
                    MessageFactory message)
        : name_(name), description_(description), serverRequired_(serverRequired),
          message_(std::move(message)) {}

    const char* name() const override { return name_; }
    const char* description() const override { return description_; }

    void configure(CLI::App& app) override {
        CLI::Option* option = app.add_option("server_url", serverUrl_,
                                             "Server base URL, e.g. https://astral.example.com");
        if (serverRequired_) {
            option->required();
        }
    }

    int execute(const CommandContext& context) override {
        throw core::AstralError(core::Errc::CommandNotImplemented, message_(context, serverUrl_));
    }

private:
    const char* name_;
    const char* description_;
    bool serverRequired_;
    MessageFactory message_;
    std::string serverUrl_;
};

} // namespace

std::unique_ptr<Command> makeLoginCommand() {
    return std::make_unique<AuthStubCommand>(
        "login", "Log in to an Astral server (device flow)", /*serverRequired=*/true,
        [](const CommandContext&, const std::string& serverUrl) {
            return "device-flow login lands with the auth client (see ARCHITECTURE.md section "
                   "6); target: " +
                   serverUrl;
        });
}

std::unique_ptr<Command> makeLogoutCommand() {
    return std::make_unique<AuthStubCommand>(
        "logout", "Forget credentials for a server", /*serverRequired=*/true,
        [](const CommandContext&, const std::string&) {
            return std::string("logout lands with the auth client; it will erase the server "
                               "entry in ~/.astral-cli/credentials.json");
        });
}

std::unique_ptr<Command> makeWhoamiCommand() {
    return std::make_unique<AuthStubCommand>(
        "whoami", "Show the authenticated principal for a server", /*serverRequired=*/false,
        [](const CommandContext& context, const std::string& serverUrl) {
            const std::string target =
                serverUrl.empty() ? context.server.value_or("<bound server>") : serverUrl;
            return "whoami lands with the auth client; target: " + target;
        });
}

} // namespace astral::commands
