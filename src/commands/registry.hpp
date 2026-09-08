#pragma once

#include <memory>
#include <vector>

#include "commands/command.hpp"

namespace astral::commands {

// Builds the v0.1 command surface in a fixed order (used for --help).
std::vector<std::unique_ptr<Command>> makeBuiltinCommands();

} // namespace astral::commands
