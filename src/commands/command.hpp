#pragma once

#include <iosfwd>
#include <optional>
#include <string>

#include "CLI/CLI.hpp"

namespace astral::commands {

// Everything a command needs at execution time. Streams are injected so
// tests can capture output; flags mirror the global options resolved by
// app wiring.
struct CommandContext {
    std::ostream& out;
    std::ostream& err;

    bool json = false;  // --json: stdout is a machine contract
    bool color = false; // styling already resolved against TTY/NO_COLOR

    std::optional<std::string> server;    // --server
    std::optional<std::string> workspace; // --workspace
};

class Command {
public:
    virtual ~Command() = default;

    virtual const char* name() const = 0;
    virtual const char* description() const = 0;

    // Adds options/subcommands to this command's CLI11 node.
    virtual void configure(CLI::App& app) = 0;

    // Executes with parsed values. Throws core::AstralError for stable
    // failures; the app layer maps them to exit codes and output.
    virtual int execute(const CommandContext& context) = 0;
};

} // namespace astral::commands
