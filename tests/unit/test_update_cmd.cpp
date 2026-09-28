#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include "app/app.hpp"
#include "commands/command.hpp"
#include "commands/update_cmd.hpp"
#include "core/exit_codes.hpp"

namespace {

// Same argv-shape helper as test_cli.cpp: builds a mutable argv view over
// literal arguments.
class Args {
public:
    explicit Args(std::vector<std::string> words) {
        for (std::string& word : words) {
            owned_.push_back(std::move(word));
        }
        for (const std::string& word : owned_) {
            argv_.push_back(const_cast<char*>(word.c_str()));
        }
    }

    int argc() const { return static_cast<int>(argv_.size()); }
    char** data() { return argv_.data(); }

private:
    std::vector<std::string> owned_;
    std::vector<char*> argv_;
};

struct RunResult {
    int exitCode = -1;
    std::string out;
    std::string err;
};

RunResult run(std::vector<std::string> words) {
    Args args(std::move(words));
    std::ostringstream out;
    std::ostringstream err;
    const int exitCode = astral::app::runApp(args.argc(), args.data(), out, err);
    return RunResult{exitCode, out.str(), err.str()};
}

// 解析期不触网、不执行：仅构造 CLI 节点并 parse（非法输入应抛 CLI::ValidationError）。
void parse(std::vector<std::string> words) {
    Args args(std::move(words));
    auto command = astral::commands::makeUpdateCommand();
    CLI::App app{"update"};
    command->configure(app);
    app.parse(args.argc(), args.data());
}

} // namespace

TEST_CASE("update with an invalid version argument is a usage error (exit 2)",
          "[update_cmd]") {
    const RunResult result = run({"astral", "update", "not-a-version"});
    REQUIRE(result.exitCode == static_cast<int>(astral::core::ExitCode::Usage));
    REQUIRE(result.err.find("invalid version format") != std::string::npos);
}

TEST_CASE("update accepts valid version forms and the --check flag at parse time",
          "[update_cmd]") {
    REQUIRE_NOTHROW(parse({"update", "v1.2.3"}));
    REQUIRE_NOTHROW(parse({"update", "1.2.3"}));
    REQUIRE_NOTHROW(parse({"update", "--check"}));
    REQUIRE_NOTHROW(parse({"update", "--check", "v0.2.1"}));
}

TEST_CASE("update still rejects malformed versions with --check", "[update_cmd]") {
    REQUIRE_THROWS_AS(parse({"update", "--check", "1.2"}), CLI::ValidationError);
}

TEST_CASE("update is registered as a builtin command", "[update_cmd]") {
    auto command = astral::commands::makeUpdateCommand();
    REQUIRE(command->name() == std::string("update"));
    REQUIRE(std::string(command->description()).size() > 0);
}
