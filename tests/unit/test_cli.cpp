#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "app/app.hpp"
#include "core/exit_codes.hpp"

namespace {

// Builds a mutable argv-style view over literal arguments.
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

} // namespace

TEST_CASE("astral version prints human output and exits 0") {
    const auto result = run({"astral", "version"});
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.out.find("astral ") == 0);
    REQUIRE(result.out.find("protocol 1") != std::string::npos);
}

TEST_CASE("astral version --json pins the machine contract") {
    const auto result = run({"astral", "version", "--json"});
    REQUIRE(result.exitCode == 0);

    const auto payload = nlohmann::json::parse(result.out);
    REQUIRE(payload.at("name") == "astral");
    REQUIRE(payload.at("protocolVersion") == 1);
    REQUIRE(payload.contains("version"));
    REQUIRE(payload.contains("platform"));
    REQUIRE(payload.contains("git"));
}

TEST_CASE("the --version flag prints the version and exits 0") {
    const auto result = run({"astral", "--version"});
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.out.find("astral ") == 0);
}

TEST_CASE("unknown subcommand is a usage error (exit 2)") {
    const auto result = run({"astral", "definitely-not-a-command"});
    REQUIRE(result.exitCode == static_cast<int>(astral::core::ExitCode::Usage));
    REQUIRE_FALSE(result.err.empty());
}

TEST_CASE("no subcommand is a usage error (exit 2)") {
    const auto result = run({"astral"});
    REQUIRE(result.exitCode == static_cast<int>(astral::core::ExitCode::Usage));
}

TEST_CASE("stub commands fail with a stable JSON error code") {
    const auto result = run({"astral", "todo", "list", "--json"});
    REQUIRE(result.exitCode == static_cast<int>(astral::core::ExitCode::GenericFailure));

    const auto payload = nlohmann::json::parse(result.out);
    REQUIRE(payload.at("error").at("code") == "COMMAND_NOT_IMPLEMENTED");
}

TEST_CASE("login without server argument is a usage error") {
    const auto result = run({"astral", "login"});
    REQUIRE(result.exitCode == static_cast<int>(astral::core::ExitCode::Usage));
}

TEST_CASE("doctor --json emits a check array") {
    const auto result = run({"astral", "doctor", "--json"});
    REQUIRE(result.exitCode == 0);

    const auto payload = nlohmann::json::parse(result.out);
    REQUIRE(payload.contains("checks"));
    REQUIRE(payload.at("checks").is_array());
    REQUIRE(payload.at("checks").size() >= 5);
    for (const auto& check : payload.at("checks")) {
        REQUIRE(check.contains("name"));
        REQUIRE(check.contains("status"));
    }
}
