#include "core/error.hpp"

namespace astral::core {

AstralError::AstralError(Errc code, std::string message)
    : std::runtime_error(message), code_(code) {}

AstralError& AstralError::withProtocol(std::string protocolCode,
                                       std::optional<std::string> requestId,
                                       std::optional<bool> retryable) {
    protocolCode_ = std::move(protocolCode);
    requestId_ = std::move(requestId);
    retryable_ = retryable;
    return *this;
}

std::string_view AstralError::codeString() const noexcept {
    switch (code_) {
    case Errc::AuthRequired:
        return "AUTH_REQUIRED";
    case Errc::ServerNotFound:
        return "SERVER_NOT_FOUND";
    case Errc::ProtocolIncompatible:
        return "PROTOCOL_INCOMPATIBLE";
    case Errc::WorkspaceNotFound:
        return "WORKSPACE_NOT_FOUND";
    case Errc::WorkspaceAlreadyBound:
        return "WORKSPACE_ALREADY_BOUND";
    case Errc::LocalWorkspaceError:
        return "LOCAL_WORKSPACE_ERROR";
    case Errc::CredentialStoreError:
        return "CREDENTIAL_STORE_ERROR";
    case Errc::NetworkError:
        return "NETWORK_ERROR";
    case Errc::Timeout:
        return "TIMEOUT";
    case Errc::CommandNotImplemented:
        return "COMMAND_NOT_IMPLEMENTED";
    case Errc::NotFound:
        return "NOT_FOUND";
    case Errc::Conflict:
        return "CONFLICT";
    case Errc::InsufficientScope:
        return "INSUFFICIENT_SCOPE";
    case Errc::Usage:
        return "USAGE";
    case Errc::Internal:
        return "INTERNAL";
    }
    return "INTERNAL";
}

int AstralError::exitCode() const noexcept {
    switch (code_) {
    case Errc::AuthRequired:
    case Errc::InsufficientScope:
        return static_cast<int>(ExitCode::Auth);
    case Errc::ServerNotFound:
    case Errc::WorkspaceNotFound:
    case Errc::NotFound:
        return static_cast<int>(ExitCode::NotFound);
    case Errc::WorkspaceAlreadyBound:
    case Errc::Conflict:
        return static_cast<int>(ExitCode::Conflict);
    case Errc::NetworkError:
        return static_cast<int>(ExitCode::Network);
    case Errc::Timeout:
        return static_cast<int>(ExitCode::Timeout);
    case Errc::LocalWorkspaceError:
        return static_cast<int>(ExitCode::LocalWorkspace);
    case Errc::Usage:
        return static_cast<int>(ExitCode::Usage);
    case Errc::ProtocolIncompatible:
        return static_cast<int>(ExitCode::Protocol);
    case Errc::CommandNotImplemented:
    case Errc::CredentialStoreError:
    case Errc::Internal:
        return static_cast<int>(ExitCode::GenericFailure);
    }
    return static_cast<int>(ExitCode::GenericFailure);
}

} // namespace astral::core
