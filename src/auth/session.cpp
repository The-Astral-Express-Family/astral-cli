#include "auth/session.hpp"

#include <thread>

#include <nlohmann/json.hpp>

#include "core/env.hpp"
#include "core/error.hpp"
#include "core/version.hpp"
#include "workspace/target.hpp"

namespace astral::auth {

namespace {

using nlohmann::json;

} // namespace

HttpFn realHttp(std::chrono::milliseconds requestTimeout) {
    return [requestTimeout](const client::HttpRequest& request) {
        client::HttpClient::Options options;
        options.requestTimeout = requestTimeout;
        client::HttpClient client{options};
        return client.send(request);
    };
}

void realSleep(std::chrono::milliseconds duration) {
    std::this_thread::sleep_for(duration);
}

namespace {

[[noreturn]] void throwProtocol(const std::string& detail) {
    throw core::AstralError(core::Errc::ProtocolIncompatible, detail);
}

std::string requireString(const json& body, const char* key, const std::string& what) {
    const auto it = body.find(key);
    if (it == body.end() || !it->is_string() || it->get<std::string>().empty()) {
        throwProtocol(what + ": missing field '" + key + "'");
    }
    return it->get<std::string>();
}

} // namespace

std::string ServerInfo::origin() const {
    const std::size_t schemeEnd = baseUrl.find("://");
    const std::size_t pathStart =
        baseUrl.find('/', schemeEnd == std::string::npos ? 0 : schemeEnd + 3);
    return pathStart == std::string::npos ? baseUrl : baseUrl.substr(0, pathStart);
}

ServerInfo discoverServer(const std::string& serverUrl, const HttpFn& http) {
    const auto normalized = workspace::normalizeServerUrl(serverUrl);
    if (!normalized) {
        throw core::AstralError(core::Errc::LocalWorkspaceError,
                                "'" + serverUrl + "' is not a server URL (scheme required)");
    }
    ServerInfo info;
    info.baseUrl = *normalized;

    client::HttpRequest request;
    request.url = info.baseUrl + "/.well-known/astral";
    const client::HttpResponse response = http(request);
    if (response.status != 200) {
        throw core::AstralError(core::Errc::ServerNotFound,
                                "no astral server at " + info.baseUrl + " (well-known status " +
                                    std::to_string(response.status) + ")");
    }
    json wellKnown;
    try {
        wellKnown = json::parse(response.body);
    } catch (const std::exception&) {
        throwProtocol("well-known: response is not valid JSON");
    }
    const int protocolVersion = wellKnown.value("protocol_version", 0);
    if (protocolVersion != core::kProtocolVersion) {
        throwProtocol("server speaks protocol " + std::to_string(protocolVersion) +
                      ", this CLI speaks " + std::to_string(core::kProtocolVersion));
    }
    info.serverId = requireString(wellKnown, "server_id", "well-known");
    info.apiBase = wellKnown.value("api_base", std::string("/api/v1"));
    return info;
}

std::string resolveServerUrl(const std::optional<std::string>& positional,
                             const std::optional<std::string>& flag) {
    std::optional<std::string> candidate = positional ? positional : flag;
    // CLI11 的可选 positional 缺席时表现为空串；视为未提供。
    if (candidate && candidate->empty()) {
        candidate.reset();
    }
    if (!candidate) {
        candidate = core::env::get("ASTRAL_SERVER");
    }
    if (!candidate) {
        throw core::AstralError(core::Errc::LocalWorkspaceError,
                                "no server given: pass a server URL, --server, "
                                "or set ASTRAL_SERVER");
    }
    const auto normalized = workspace::normalizeServerUrl(*candidate);
    if (!normalized) {
        throw core::AstralError(core::Errc::LocalWorkspaceError,
                                "'" + *candidate + "' is not a server URL (scheme required)");
    }
    return *normalized;
}

platform::LoginSession requireSession(platform::CredentialStore& store,
                                      const std::string& serverUrl) {
    auto session = store.loadSession(serverUrl);
    if (!session) {
        throw core::AstralError(core::Errc::AuthRequired,
                                "not logged in to " + serverUrl + "; run astral login");
    }
    return *session;
}

client::HttpResponse
withLazyRefresh(platform::CredentialStore& store, const HttpFn& http,
                platform::LoginSession& session,
                const std::function<client::HttpResponse(const platform::LoginSession&)>& call) {
    client::HttpResponse response = call(session);
    if (response.status != 401) {
        return response;
    }

    client::HttpRequest refresh;
    refresh.method = "POST";
    refresh.url = ServerInfo{session.serverUrl, "", session.apiBase}.origin() + session.apiBase +
                  "/auth/token/refresh";
    refresh.body = json{{"refresh_token", session.refreshToken}}.dump();
    const client::HttpResponse rotated = http(refresh);

    if (rotated.status == 401) {
        // Refresh replay = family revoked server-side; the local pair is dead.
        store.eraseSession(session.serverUrl);
        throw core::AstralError(core::Errc::AuthRequired,
                                "session expired or revoked; run astral login");
    }
    if (rotated.status != 200) {
        throw core::AstralError(core::Errc::NetworkError, "token refresh failed with status " +
                                                              std::to_string(rotated.status));
    }
    json pair;
    try {
        pair = json::parse(rotated.body);
    } catch (const std::exception&) {
        throwProtocol("token refresh: response is not valid JSON");
    }
    const auto access = pair.find("access_token");
    const auto refresh_ = pair.find("refresh_token");
    if (access == pair.end() || refresh_ == pair.end()) {
        throwProtocol("token refresh: missing token fields");
    }
    session.accessToken = access->get<std::string>();
    session.refreshToken = refresh_->get<std::string>();
    store.saveSession(session.serverUrl, session);
    return call(session);
}

} // namespace astral::auth
