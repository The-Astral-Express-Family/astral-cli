#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>

#include "client/http_client.hpp"
#include "platform/credential_store.hpp"

namespace astral::auth {

// Transport seam: lets unit tests script HTTP responses without libcurl.
using HttpFn = std::function<client::HttpResponse(const client::HttpRequest&)>;

// Wall-clock seam for polling waits; unit tests record durations instead.
using SleepFn = std::function<void(std::chrono::milliseconds)>;

// libcurl-backed defaults for command layers.
HttpFn realHttp(std::chrono::milliseconds requestTimeout = std::chrono::seconds{30});
void realSleep(std::chrono::milliseconds duration);

// Result of discovery against /.well-known/astral.
struct ServerInfo {
    std::string baseUrl;  // normalized input URL
    std::string serverId; // srv_...
    std::string apiBase;  // e.g. /api/v1

    // scheme://host[:port] — api_base is server-rooted.
    std::string origin() const;
};

// GET /.well-known/astral and gate the protocol version. Throws
// SERVER_NOT_FOUND (not an astral server) / PROTOCOL_INCOMPATIBLE.
ServerInfo discoverServer(const std::string& serverUrl, const HttpFn& http);

// Server resolution for auth commands: positional > --server flag >
// ASTRAL_SERVER env. Throws LOCAL_WORKSPACE_ERROR with guidance when empty or
// scheme-less.
std::string resolveServerUrl(const std::optional<std::string>& positional,
                             const std::optional<std::string>& flag);

// Stored session or AUTH_REQUIRED("not logged in to <url>; run astral login").
platform::LoginSession requireSession(platform::CredentialStore& store,
                                      const std::string& serverUrl);

// D13 (modulator TODO §11): authenticated calls get exactly one lazy refresh.
// `call(session)` performs one request; on 401 the session pair is rotated via
// /auth/token/refresh, persisted, and the call replays once. A refresh that is
// itself 401 means the family was revoked: the local session is erased and
// AUTH_REQUIRED thrown. Other failures surface as-is.
client::HttpResponse
withLazyRefresh(platform::CredentialStore& store, const HttpFn& http,
                platform::LoginSession& session,
                const std::function<client::HttpResponse(const platform::LoginSession&)>& call);

} // namespace astral::auth
