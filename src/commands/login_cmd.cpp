#include "commands/login_cmd.hpp"

#include <ostream>

#include "core/error.hpp"

namespace astral::commands {

namespace {

// Login/logout/whoami need device-flow authorization against a live server
// (ARCHITECTURE.md section 6). The v0.1 scaffold wires the command surface
// and refuses cleanly instead of half-implementing an auth flow.
class LoginCommand final : public Command {
public:
    const char* name() const override { return "login"; }
    const char* description() const override { return "Log in to an Astral server (device flow)"; }

    void configure(CLI::App& app) override {
        app.add_option("server_url", serverUrl_, "Server base URL, e.g. https://astral.example.com")
            ->required();
    }

    int execute(const CommandContext& context) override {
        (void)context;
        throw core::AstralError(core::Errc::CommandNotImplemented,
                                "device-flow login lands with the auth client (see "
                                "ARCHITECTURE.md section 6); target: " +
                                    serverUrl_);
    }

private:
    std::string serverUrl_;
};

class LogoutCommand final : public Command {
public:
    const char* name() const override { return "logout"; }
    const char* description() const override { return "Forget credentials for a server"; }

    void configure(CLI::App& app) override {
        app.add_option("server_url", serverUrl_, "Server base URL")->required();
    }

    int execute(const CommandContext& context) override {
        (void)context;
        throw core::AstralError(core::Errc::CommandNotImplemented,
                                "logout lands with the auth client; it will erase the server "
                                "entry in ~/.astral-cli/credentials.json; target: " +
                                    serverUrl_);
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
        app.add_option("server_url", serverUrl_, "Server base URL (default: bound server)");
    }

    int execute(const CommandContext& context) override {
        (void)context;
        const std::string target =
            serverUrl_.empty() ? context.server.value_or("<bound server>") : serverUrl_;
        throw core::AstralError(core::Errc::CommandNotImplemented,
                                "whoami lands with the auth client; target: " + target);
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
