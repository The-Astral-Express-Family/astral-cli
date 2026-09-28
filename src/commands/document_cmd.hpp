#pragma once

#include <memory>

#include "commands/command.hpp"

namespace astral::commands {

std::unique_ptr<Command> makeDocumentCommand();

} // namespace astral::commands
