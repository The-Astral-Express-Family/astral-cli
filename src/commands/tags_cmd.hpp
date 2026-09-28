#pragma once

#include <memory>

#include "commands/command.hpp"

namespace astral::commands {

// `astral tags ...` (ARCHITECTURE.md section 11): workspace tag dictionary
// with the two-step proposal/confirm flow (spelling pinned to --confirm).
std::unique_ptr<Command> makeTagsCommand();

} // namespace astral::commands
