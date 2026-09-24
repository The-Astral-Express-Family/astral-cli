#include "auth/api.hpp"

#include <filesystem>
#include <utility>

#include <nlohmann/json.hpp>

#include "core/env.hpp"
#include "core/error.hpp"
#include "workspace/resolve.hpp"

namespace astral::auth {

namespace {

using nlohmann::json;

// Exit-code mapping for server failures. Remaining 4xx codes (e.g.
// VALIDATION_FAILED on 400) map to PROTOCOL_INCOMPATIBLE: the CLI believed
// the request valid, so a 4xx most likely means client/server drift — the
// protocol code passthrough keeps the precise cause visible.
core::Errc errcForStatus(long status) {
    if (status == 401) {
        return core::Errc::AuthRequired;
    }
    if (status == 403) {
        return core::Errc::InsufficientScope;
    }
    if (status == 404) {
        return core::Errc::NotFound;
    }
    if (status == 409) {
        return core::Errc::Conflict;
    }
    if (status >= 500) {
        return core::Errc::NetworkError;
    }
    return core::Errc::ProtocolIncompatible;
}

[[noreturn]] void throwProtocol(const std::string& detail) {
    throw core::AstralError(core::Errc::ProtocolIncompatible, detail);
}

} // namespace

LocalTarget resolveLocalTarget(const std::optional<std::string>& flagServer,
                               const std::optional<std::string>& flagWorkspace) {
    workspace::ResolveInput input;
    input.flagServer = flagServer;
    input.flagWorkspace = flagWorkspace;
    if (auto bindingDir = workspace::findBindingDir(std::filesystem::current_path())) {
        input.localBinding = workspace::readBinding(*bindingDir);
    }
    input.envServer = core::env::get("ASTRAL_SERVER");
    input.envWorkspace = core::env::get("ASTRAL_WORKSPACE");

    const workspace::ResolveResult resolved = workspace::resolveTarget(input);
    if (!resolved.ok) {
        throw core::AstralError(core::Errc::LocalWorkspaceError, resolved.failureReason);
    }
    LocalTarget local;
    local.serverUrl = resolved.target.serverUrl;
    local.workspaceId = resolved.target.workspaceId;
    local.workspaceName = resolved.target.workspaceName;
    return local;
}

ApiSession::ApiSession(const std::string& serverUrl)
    : server_(discoverServer(serverUrl, commandHttp())),
      store_(platform::makeDefaultCredentialStore()) {}

client::HttpResponse ApiSession::send(client::HttpRequest request) const {
    const HttpFn http = commandHttp();
    stampClientHeaders(request);
    // Agent credential via environment wins outright and never refreshes.
    if (auto envToken = core::env::get("ASTRAL_TOKEN"); envToken && !envToken->empty()) {
        request.bearerToken = *envToken;
        return http(request);
    }
    if (!session_) {
        session_ = requireSession(*store_, server_.baseUrl);
    }
    return withLazyRefresh(*store_, http, *session_,
                           [&request, &http](const platform::LoginSession& s) {
                               // Copy, never move: withLazyRefresh replays
                               // this call after a token rotation.
                               client::HttpRequest authed = request;
                               authed.bearerToken = s.accessToken;
                               return http(authed);
                           });
}

client::HttpResponse ApiSession::requireSuccess(client::HttpRequest request,
                                                const std::string& what) const {
    const client::HttpResponse response = send(std::move(request));
    if (response.status < 200 || response.status >= 300) {
        throwApiError(response, what);
    }
    return response;
}

std::string apiUrl(const ApiSession& api, const std::string& path) {
    return api.server().origin() + api.server().apiBase + path;
}

nlohmann::json getJson(const ApiSession& api, const std::string& path, const std::string& what) {
    client::HttpRequest request;
    request.url = apiUrl(api, path);
    return json::parse(api.requireSuccess(std::move(request), what).body);
}

nlohmann::json sendJson(const ApiSession& api, std::string method, const std::string& path,
                        const nlohmann::json& body, const std::string& what,
                        std::vector<std::pair<std::string, std::string>> extraHeaders) {
    client::HttpRequest request;
    request.method = std::move(method);
    request.url = apiUrl(api, path);
    request.body = body.dump();
    request.headers.emplace_back("Content-Type", "application/json");
    for (auto& [name, value] : extraHeaders) {
        request.headers.emplace_back(std::move(name), std::move(value));
    }
    return json::parse(api.requireSuccess(std::move(request), what).body);
}

client::HttpResponse sessionGet(platform::CredentialStore& store, platform::LoginSession& session,
                                const std::string& url) {
    const HttpFn http = commandHttp();
    const auto call = [&http, &url](const platform::LoginSession& s) {
        client::HttpRequest request;
        request.url = url;
        request.bearerToken = s.accessToken;
        stampClientHeaders(request);
        return http(request);
    };
    return withLazyRefresh(store, http, session, call);
}

client::HttpResponse sessionPost(platform::CredentialStore& store, platform::LoginSession& session,
                                 const std::string& url, const std::string& jsonBody) {
    const HttpFn http = commandHttp();
    const auto call = [&http, &url, &jsonBody](const platform::LoginSession& s) {
        client::HttpRequest request;
        request.method = "POST";
        request.url = url;
        request.bearerToken = s.accessToken;
        request.body = jsonBody;
        stampClientHeaders(request);
        return http(request);
    };
    return withLazyRefresh(store, http, session, call);
}

void throwApiError(const client::HttpResponse& response, const std::string& what) {
    std::string protocolCode;
    std::string detail;
    std::optional<std::string> requestId;
    std::optional<bool> retryable;
    try {
        const json envelope = json::parse(response.body).at("error");
        protocolCode = envelope.value("code", std::string());
        detail = envelope.value("message", std::string());
        if (auto it = envelope.find("request_id"); it != envelope.end() && it->is_string()) {
            requestId = it->get<std::string>();
        }
        if (auto it = envelope.find("retryable"); it != envelope.end() && it->is_boolean()) {
            retryable = it->get<bool>();
        }
    } catch (const std::exception&) {
        // Non-JSON or malformed body: fall through to status-only mapping.
    }
    std::string message = detail.empty()
                              ? what + " failed with HTTP status " + std::to_string(response.status)
                              : what + ": " + detail;
    core::AstralError error{errcForStatus(response.status), std::move(message)};
    if (!protocolCode.empty()) {
        error.withProtocol(std::move(protocolCode), std::move(requestId), std::move(retryable));
    }
    throw error;
}

WorkspaceContext resolveWorkspace(ApiSession& api, const LocalTarget& local) {
    WorkspaceContext context;
    context.server = api.server();
    if (!local.workspaceId.empty()) {
        context.workspaceId = local.workspaceId;
        context.workspaceName = local.workspaceName;
        return context;
    }
    if (local.workspaceName.empty()) {
        throwProtocol("workspace target has neither id nor name");
    }

    // Name-only target (flag/env): exact-name lookup. §10.9 semantics —
    // empty items means missing OR invisible; both surface as 404 to the
    // caller so users cannot probe workspace existence.
    client::HttpRequest request;
    request.url = apiUrl(api, "/workspaces?name=" + client::urlEncode(local.workspaceName));
    const json items = json::parse(api.requireSuccess(std::move(request), "workspace lookup").body)
                           .value("items", json::array());
    if (items.empty() || !items.front().is_object()) {
        throw core::AstralError(core::Errc::WorkspaceNotFound, "workspace '" + local.workspaceName +
                                                                   "' not found on " +
                                                                   api.server().baseUrl);
    }
    context.workspaceId = items.front().value("id", std::string());
    if (context.workspaceId.empty()) {
        throwProtocol("workspace lookup: response missing id");
    }
    context.workspaceName = items.front().value("name", local.workspaceName);
    return context;
}

std::pair<ApiSession, WorkspaceContext>
openWorkspace(const std::optional<std::string>& flagServer,
              const std::optional<std::string>& flagWorkspace) {
    try {
        const LocalTarget local = resolveLocalTarget(flagServer, flagWorkspace);
        ApiSession api{local.serverUrl};
        WorkspaceContext ws = resolveWorkspace(api, local);
        return {std::move(api), std::move(ws)};
    } catch (const core::AstralError& e) {
        // D14 (modulator TODO §1): root-level/personal tasks live in a
        // default/<user>/todo workspace; surface the convention wherever the
        // workspace target is missing or unresolvable.
        if (e.code() == core::Errc::LocalWorkspaceError ||
            e.code() == core::Errc::WorkspaceNotFound) {
            throw core::AstralError(
                e.code(),
                std::string(e.what()) +
                    " (tip: keep personal tasks in a 'default/<your-name>/todo' "
                    "workspace; create one with `astral init <server-url>/todo --create`)");
        }
        throw;
    }
}

nlohmann::json fetchPageItems(const ApiSession& api, const std::string& path, std::string query,
                              bool followAll, const std::string& what, std::string& nextCursor) {
    nlohmann::json items = nlohmann::json::array();
    while (true) {
        client::HttpRequest request;
        request.url = apiUrl(api, query.empty() ? path : path + "?" + query);
        const nlohmann::json body =
            nlohmann::json::parse(api.requireSuccess(std::move(request), what).body);
        if (auto it = body.find("items"); it != body.end() && it->is_array()) {
            for (const auto& item : *it) {
                items.push_back(item);
            }
        }
        const auto next = body.find("next_cursor");
        const bool hasMore =
            next != body.end() && next->is_string() && !next->get<std::string>().empty();
        nextCursor = hasMore ? next->get<std::string>() : std::string();
        if (!hasMore || !followAll) {
            return items;
        }
        if (!query.empty()) {
            query += '&';
        }
        query += "cursor=" + client::urlEncode(nextCursor);
    }
}

} // namespace astral::auth
