#pragma once

#include <memory>

#include "commands/command.hpp"

namespace astral::commands {

std::unique_ptr<Command> makeLoginCommand();
std::unique_ptr<Command> makeLogoutCommand();
std::unique_ptr<Command> makeWhoamiCommand();

} // namespace astral::commands
