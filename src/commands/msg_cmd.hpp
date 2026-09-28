#pragma once

#include <memory>

#include "commands/command.hpp"

namespace astral::commands {

// `astral msg ...` (ARCHITECTURE.md section 11): workspace messages with
// actor/workspace/task targets and thread follow-ups.
std::unique_ptr<Command> makeMsgCommand();

} // namespace astral::commands
