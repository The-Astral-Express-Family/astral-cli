#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

#include "core/exit_codes.hpp"

namespace astral::core {

// Machine-facing error kinds. The string form is the stable `error.code`
// promised to Agent/JSON callers; never rename existing values.
enum class Errc {
    AuthRequired,          // AUTH_REQUIRED
    ServerNotFound,        // SERVER_NOT_FOUND
    ProtocolIncompatible,  // PROTOCOL_INCOMPATIBLE
    WorkspaceNotFound,     // WORKSPACE_NOT_FOUND
    WorkspaceAlreadyBound, // WORKSPACE_ALREADY_BOUND
    LocalWorkspaceError,   // LOCAL_WORKSPACE_ERROR
    CredentialStoreError,  // CREDENTIAL_STORE_ERROR
    NetworkError,          // NETWORK_ERROR
    Timeout,               // TIMEOUT
    CommandNotImplemented, // COMMAND_NOT_IMPLEMENTED
    Internal,              // INTERNAL
};

class AstralError : public std::runtime_error {
public:
    AstralError(Errc code, std::string message);

    Errc code() const noexcept { return code_; }
    std::string_view codeString() const noexcept;
    int exitCode() const noexcept;

private:
    Errc code_;
};

} // namespace astral::core
