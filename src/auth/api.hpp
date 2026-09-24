#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

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

// GET shorthand: requireSuccess + JSON parse of the body.
nlohmann::json getJson(const ApiSession& api, const std::string& path, const std::string& what);

// JSON-body request shorthand (POST/PATCH/...): method, Content-Type and
// serialized body are assembled here, then requireSuccess + JSON parse.
// `extraHeaders` carries per-request protocol headers (Idempotency-Key).
nlohmann::json sendJson(const ApiSession& api, std::string method, const std::string& path,
                        const nlohmann::json& body, const std::string& what,
                        std::vector<std::pair<std::string, std::string>> extraHeaders = {});

// Body-free request shorthand (DELETE lease/tag, bodyless PUT): method is
// assembled here, then requireSuccess. The raw response comes back unparsed
// so empty 204 bodies (and bodies the caller must interpret) stay workable.
client::HttpResponse sendNoBody(const ApiSession& api, std::string method, const std::string& path,
                                const std::string& what);

// Session-authenticated GET/POST for the human-only commands (whoami, init):
// unlike ApiSession these deliberately never honor ASTRAL_TOKEN — binding a
// workspace or asking "who am I" must act as the logged-in human, not an
// ambient agent credential. One lazy refresh per call (D13).
client::HttpResponse sessionGet(platform::CredentialStore& store, platform::LoginSession& session,
                                const std::string& url);
client::HttpResponse sessionPost(platform::CredentialStore& store, platform::LoginSession& session,
                                 const std::string& url, const std::string& jsonBody);

// Fully resolves a workspace for a command: local target first, then an
// exact-name lookup over the API when only a name is known (flag/env path).
// Missing or invisible workspaces throw WORKSPACE_NOT_FOUND.
struct WorkspaceContext {
    ServerInfo server;
    std::string workspaceId;
    std::string workspaceName;
};

WorkspaceContext resolveWorkspace(ApiSession& api, const LocalTarget& local);

// Combined preamble for workspace-scoped commands: local target resolution
// (flag > binding > env), one session (exactly one discovery), then workspace
// resolution. A bare LOCAL_WORKSPACE_ERROR / WORKSPACE_NOT_FOUND gets the D14
// default-workspace hint appended (personal tasks live in
// default/<your-name>/todo).
std::pair<ApiSession, WorkspaceContext>
openWorkspace(const std::optional<std::string>& flagServer,
              const std::optional<std::string>& flagWorkspace);

// Fetches {path}[?query] page by page, accumulating the `items` arrays.
// Opaque next_cursor values feed &cursor=...; followAll walks until exhausted.
// `nextCursor` receives the last observed cursor ("" when exhausted).
nlohmann::json fetchPageItems(const ApiSession& api, const std::string& path, std::string query,
                              bool followAll, const std::string& what, std::string& nextCursor);

// Maps a non-2xx response to AstralError: exit code from the HTTP status,
// message from the protocol envelope, and the server's error.code plus
// request_id/retryable carried through for --json (ARCHITECTURE.md 12).
[[noreturn]] void throwApiError(const client::HttpResponse& response, const std::string& what);

} // namespace astral::auth
