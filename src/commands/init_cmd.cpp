#include "commands/init_cmd.hpp"

#include <ostream>

#include "core/error.hpp"
#include "workspace/target.hpp"

namespace astral::commands {

namespace {

// `astral init <server_url>[/<workspace_name>] [path]` follows the state
// machine in ARCHITECTURE.md section 9.3. The scaffold implements the pure
// input-parsing half (and exposes it for tests); the discovery/credential/
// write phases land in the login/init round (modulator TODO §11 D12/D13).
class InitCommand final : public Command {
public:
    const char* name() const override { return "init"; }
    const char* description() const override { return "Bind a directory to an Astral workspace"; }

    void configure(CLI::App& app) override {
        app.add_option("server_url", serverRef_, "Server URL, optionally with /<workspace_name>")
            ->required();
        app.add_option("path", path_, "Directory to bind (default: current directory)");
        app.add_option("--workspace,-w", workspace_,
                       "Explicit workspace name (disables shorthand "
                       "splitting)");
        app.add_flag("--create", create_, "Create the workspace if missing (non-interactive)");
        app.add_flag("--rebind", rebind_, "Replace an existing different binding");
    }

    int execute(const CommandContext& context) override {
        (void)context;
        const auto explicitWorkspace = workspace_.empty() ? std::optional<std::string>{}
                                                          : std::optional<std::string>{workspace_};
        const workspace::TargetSpec spec =
            workspace::parseTargetSpec(serverRef_, explicitWorkspace);
        const std::string note = spec.workspaceName
                                     ? "workspace candidate '" + *spec.workspaceName + "'"
                                     : std::string("no workspace candidate");
        throw core::AstralError(core::Errc::CommandNotImplemented,
                                "init's discovery/binding phases land in the login/init round; "
                                "parsed target: server '" +
                                    spec.fullUrl + "', " + note);
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
