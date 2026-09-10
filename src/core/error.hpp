#pragma once

#include <optional>
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
    NotFound,              // NOT_FOUND (a server resource does not exist)
    Conflict,              // CONFLICT (server returned 409)
    InsufficientScope,     // INSUFFICIENT_SCOPE (server returned 403)
    Usage,                 // USAGE (bad input that CLI11 alone can't express)
    Internal,              // INTERNAL
};

class AstralError : public std::runtime_error {
public:
    AstralError(Errc code, std::string message);

    // Attaches the server's verbatim protocol error envelope fields
    // (ARCHITECTURE.md section 12): --json failures then surface
    // {"error":{"code": <protocol code>, "message", "request_id",
    // "retryable"}} so agents can branch on the frozen contract. CLI-local
    // failures carry no protocol fields and keep the CLI-local code.
    AstralError& withProtocol(std::string protocolCode, std::optional<std::string> requestId,
                              std::optional<bool> retryable);

    Errc code() const noexcept { return code_; }
    std::string_view codeString() const noexcept;
    int exitCode() const noexcept;

    // Protocol passthrough; nullopt when the failure did not come from a
    // server error envelope.
    const std::optional<std::string>& protocolCode() const noexcept { return protocolCode_; }
    const std::optional<std::string>& requestId() const noexcept { return requestId_; }
    const std::optional<bool>& retryable() const noexcept { return retryable_; }

private:
    Errc code_;
    std::optional<std::string> protocolCode_;
    std::optional<std::string> requestId_;
    std::optional<bool> retryable_;
};

} // namespace astral::core
