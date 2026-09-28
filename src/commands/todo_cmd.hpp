#pragma once

#include <memory>

#include "commands/command.hpp"

namespace astral::commands {

// `astral todo ...` (ARCHITECTURE.md section 11): the server task tree is
// the source of truth; the CLI offers friendly verbs over it.
std::unique_ptr<Command> makeTodoCommand();

} // namespace astral::commands
