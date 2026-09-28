#pragma once

#include <memory>

#include "commands/command.hpp"

namespace astral::commands {

// `astral event listen` (ARCHITECTURE.md section 11): consumes the workspace
// SSE stream as JSON Lines with automatic resume and reconnect.
std::unique_ptr<Command> makeEventCommand();

} // namespace astral::commands
