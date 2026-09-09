#include "commands/version_cmd.hpp"

#include <ostream>

#include <nlohmann/json.hpp>

#include "core/version.hpp"
#include "output/json_output.hpp"

namespace astral::commands {

namespace {

class VersionCommand final : public Command {
public:
    const char* name() const override { return "version"; }
    const char* description() const override { return "Print CLI version details"; }

    void configure(CLI::App& app) override { (void)app; }

    int execute(const CommandContext& context) override {
        if (context.json) {
            output::printJson(context.out, core::identityFields(/*withName=*/true));
            return 0;
        }
        context.out << core::identityString() << "\n";
        return 0;
    }
};

} // namespace

std::unique_ptr<Command> makeVersionCommand() {
    return std::make_unique<VersionCommand>();
}

} // namespace astral::commands
