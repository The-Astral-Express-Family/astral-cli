#include "app/app.hpp"

#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "CLI/CLI.hpp"
#include "commands/command.hpp"
#include "commands/registry.hpp"
#include "core/error.hpp"
#include "core/exit_codes.hpp"
#include "core/log.hpp"
#include "core/version.hpp"
#include "output/json_output.hpp"
#include "output/style.hpp"
#include "output/tty.hpp"

namespace astral::app {

namespace {

struct GlobalOptions {
    bool json = false;
    std::string color = "auto";
    std::string logLevel = "warn";
    std::string server;
    std::string workspace;
};

void printFailure(const commands::CommandContext& context, const core::AstralError& error) {
    if (context.json) {
        output::printJsonError(context.out, error.codeString(), error.what());
        return;
    }
    const output::Painter paint(context.color);
    context.err << paint.err("astral:") << " [" << error.codeString() << "] " << error.what()
                << '\n';
}

int dispatchCommand(commands::Command& command, const GlobalOptions& options, std::ostream& out,
                    std::ostream& err) {
    const bool color = output::shouldUseColor(options.color, options.json);
    core::configureLogging(options.logLevel, color && output::stderrIsTty());

    commands::CommandContext context{
        out,
        err,
        options.json,
        color,
        options.server.empty() ? std::optional<std::string>{}
                               : std::optional<std::string>{options.server},
        options.workspace.empty() ? std::optional<std::string>{}
                                  : std::optional<std::string>{options.workspace}};
    try {
        return command.execute(context);
    } catch (const core::AstralError& error) {
        printFailure(context, error);
        return error.exitCode();
    } catch (const std::exception& error) {
        const core::AstralError wrapped(core::Errc::Internal, error.what());
        printFailure(context, wrapped);
        return wrapped.exitCode();
    }
}

} // namespace

int runApp(int argc, char** argv, std::ostream& out, std::ostream& err) {
    CLI::App app{"astral - client for Astral servers", "astral"};
    app.set_version_flag("-v,--version", std::string(core::kProjectVersion));
    // 注：version 串参数实际不会输出——本程序自行捕获 CLI::CallForVersion
    // 打印加长格式（版本 + 平台 + 协议版本），CLI11 自带的 run() 未被使用。
    app.require_subcommand(1);
    // Global flags stay usable after the subcommand (e.g. `astral todo list
    // --json`), matching what users expect from modern CLIs.
    app.fallthrough(true);

    GlobalOptions options;
    app.add_flag("--json", options.json,
                 "Machine-readable JSON on stdout (diagnostics go to stderr)");
    app.add_option("--color", options.color, "Colorize output: auto | always | never")
        ->capture_default_str()
        ->check(CLI::IsMember({"auto", "always", "never"}));
    app.add_option("--log-level", options.logLevel, "stderr log level")
        ->capture_default_str()
        ->check(CLI::IsMember({"trace", "debug", "info", "warn", "err", "off"}));
    app.add_option("--server", options.server, "Explicit server URL (overrides binding)");
    app.add_option("--workspace", options.workspace, "Explicit workspace (overrides binding)");

    int exitCode = static_cast<int>(core::ExitCode::Success);
    std::vector<std::unique_ptr<commands::Command>> commands = commands::makeBuiltinCommands();
    for (const auto& command : commands) {
        CLI::App* sub = app.add_subcommand(command->name(), command->description());
        command->configure(*sub);
        commands::Command* raw = command.get();
        sub->callback([&exitCode, raw, &options, &out, &err] {
            exitCode = dispatchCommand(*raw, options, out, err);
        });
    }

    try {
        app.parse(argc, argv);
    } catch (const CLI::CallForHelp& help) {
        (void)help;
        err << app.help();
        return static_cast<int>(core::ExitCode::Success);
    } catch (const CLI::CallForVersion& version) {
        out << "astral " << core::kProjectVersion << " (" << core::buildPlatform() << ", protocol "
            << core::kProtocolVersion << ")\n";
        (void)version;
        return static_cast<int>(core::ExitCode::Success);
    } catch (const CLI::ParseError& parseError) {
        // Usage failures are exit code 2 (ARCHITECTURE.md section 12).
        err << "astral: " << parseError.what() << "\ntry 'astral --help'\n";
        return static_cast<int>(core::ExitCode::Usage);
    }

    return exitCode;
}

int runMain(int argc, char** argv) {
#ifdef _WIN32
    output::enableNativeAnsi();
#endif
    return runApp(argc, argv, std::cout, std::cerr);
}

} // namespace astral::app
