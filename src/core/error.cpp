#include "core/error.hpp"

namespace astral::core {

AstralError::AstralError(Errc code, std::string message)
    : std::runtime_error(message), code_(code) {}

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
    case Errc::Internal:
        return "INTERNAL";
    }
    return "INTERNAL";
}

int AstralError::exitCode() const noexcept {
    switch (code_) {
    case Errc::AuthRequired:
        return static_cast<int>(ExitCode::Auth);
    case Errc::ServerNotFound:
    case Errc::WorkspaceNotFound:
        return static_cast<int>(ExitCode::NotFound);
    case Errc::WorkspaceAlreadyBound:
        return static_cast<int>(ExitCode::Conflict);
    case Errc::NetworkError:
        return static_cast<int>(ExitCode::Network);
    case Errc::Timeout:
        return static_cast<int>(ExitCode::Timeout);
    case Errc::LocalWorkspaceError:
        return static_cast<int>(ExitCode::LocalWorkspace);
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
