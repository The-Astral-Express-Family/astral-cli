#pragma once

#include <memory>
#include <optional>
#include <string>

#include "auth/session.hpp"
#include "client/http_client.hpp"
#include "platform/credential_store.hpp"

namespace astral::auth {

// Locally resolved call target (ARCHITECTURE.md section 10 precedence:
// --server/--workspace flags > repo binding > env). Pure, no networking.
struct LocalTarget {
    std::string serverUrl;
    std::string workspaceId;   // from a binding; empty for flag/env targets
    std::string workspaceName; // display name; flag/env targets carry only this
};

LocalTarget resolveLocalTarget(const std::optional<std::string>& flagServer,
                               const std::optional<std::string>& flagWorkspace);

// Authenticated API session for business commands. Bearer policy
// (ARCHITECTURE.md section 6.3): ASTRAL_TOKEN (env, agent credential) wins
// over the credentials file; without it the human session slot is used with
// exactly one lazy refresh per request (D13).
class ApiSession {
public:
    // Discovers the server (well-known + protocol version gate).
    explicit ApiSession(const std::string& serverUrl);

    ApiSession(ApiSession&&) = default;
    ApiSession& operator=(ApiSession&&) = default;

    const ServerInfo& server() const { return server_; }

    // Sends with the bearer resolved per call. Transport failures surface
    // as-is (TIMEOUT/NETWORK_ERROR); non-2xx statuses are returned.
    client::HttpResponse send(client::HttpRequest request) const;

    // send + throwApiError on any non-2xx.
    client::HttpResponse requireSuccess(client::HttpRequest request, const std::string& what) const;

private:
    ServerInfo server_;
    std::unique_ptr<platform::CredentialStore> store_;
    mutable std::optional<platform::LoginSession> session_;
};

// origin + apiBase + path (path starts with '/').
std::string apiUrl(const ApiSession& api, const std::string& path);

// Fully resolves a workspace for a command: local target first, then an
// exact-name lookup over the API when only a name is known (flag/env path).
// Missing or invisible workspaces throw WORKSPACE_NOT_FOUND.
struct WorkspaceContext {
    ServerInfo server;
    std::string workspaceId;
    std::string workspaceName;
};

WorkspaceContext resolveWorkspace(ApiSession& api, const LocalTarget& local);

// Maps a non-2xx response to AstralError: exit code from the HTTP status,
// message from the protocol envelope, and the server's error.code plus
// request_id/retryable carried through for --json (ARCHITECTURE.md 12).
[[noreturn]] void throwApiError(const client::HttpResponse& response, const std::string& what);

} // namespace astral::auth
