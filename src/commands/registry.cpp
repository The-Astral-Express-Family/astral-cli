#include "commands/registry.hpp"

#include <string>
#include <utility>
#include <vector>

#include "commands/doctor_cmd.hpp"
#include "commands/init_cmd.hpp"
#include "commands/login_cmd.hpp"
#include "commands/version_cmd.hpp"
#include "core/error.hpp"

namespace astral::commands {

namespace {

// A noun whose CLI surface is wired up but whose behavior lands later.
// Keeps help text and shell completion stable while implementation follows.
class StubbedNounCommand final : public Command {
public:
    StubbedNounCommand(const char* name, const char* description,
                       std::vector<std::string> subcommands)
        : name_(name), description_(description), subcommands_(std::move(subcommands)) {}

    const char* name() const override { return name_; }
    const char* description() const override { return description_; }

    void configure(CLI::App& app) override {
        app.require_subcommand(1);
        for (const std::string& subcommand : subcommands_) {
            app.add_subcommand(subcommand,
                               std::string("'astral ") + name_ + " " + subcommand + "' (planned)");
        }
    }

    int execute(const CommandContext& context) override {
        (void)context;
        throw core::AstralError(core::Errc::CommandNotImplemented,
                                std::string("'astral ") + name_ +
                                    " ...' is not implemented yet (watch the login/init round in "
                                    "the modulator repo TODO)");
    }

private:
    const char* name_;
    const char* description_;
    std::vector<std::string> subcommands_;
};

} // namespace

std::vector<std::unique_ptr<Command>> makeBuiltinCommands() {
    std::vector<std::unique_ptr<Command>> commands;
    commands.push_back(makeLoginCommand());
    commands.push_back(makeLogoutCommand());
    commands.push_back(makeWhoamiCommand());
    commands.push_back(makeInitCommand());
    commands.push_back(std::make_unique<StubbedNounCommand>(
        "workspace", "Inspect and manage workspaces",
        std::vector<std::string>{"list", "show", "create", "archive"}));
    commands.push_back(std::make_unique<StubbedNounCommand>(
        "todo", "Work with tasks",
        std::vector<std::string>{"list", "add", "show", "claim", "done", "search"}));
    commands.push_back(std::make_unique<StubbedNounCommand>(
        "tags", "Manage tags (two-step proposal/confirm)",
        std::vector<std::string>{"list", "create", "rename", "delete"}));
    commands.push_back(std::make_unique<StubbedNounCommand>("status", "Show workspace status",
                                                            std::vector<std::string>{}));
    commands.push_back(
        std::make_unique<StubbedNounCommand>("msg", "Read and send workspace messages",
                                             std::vector<std::string>{"send", "list", "show"}));
    commands.push_back(
        std::make_unique<StubbedNounCommand>("document", "Manage workspace documents",
                                             std::vector<std::string>{"list", "show", "upsert"}));
    commands.push_back(std::make_unique<StubbedNounCommand>(
        "event", "Consume workspace event stream", std::vector<std::string>{"listen", "list"}));
    commands.push_back(std::make_unique<StubbedNounCommand>(
        "agent", "Manage agent credentials and sessions",
        std::vector<std::string>{"list", "register", "revoke"}));
    commands.push_back(makeDoctorCommand());
    commands.push_back(makeVersionCommand());
    return commands;
}

} // namespace astral::commands
