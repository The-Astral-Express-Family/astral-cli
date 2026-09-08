#pragma once

namespace astral::core {

// Stable process exit codes; see ARCHITECTURE.md section 12.
enum class ExitCode : int {
    Success = 0,
    GenericFailure = 1,
    Usage = 2,
    Auth = 3,
    NotFound = 4,
    Conflict = 5,
    Network = 6,
    Timeout = 7,
    LocalWorkspace = 8,
    Protocol = 9,
};

} // namespace astral::core
