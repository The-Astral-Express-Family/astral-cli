#include "auth/device_flow.hpp"

#include <string>

#include <nlohmann/json.hpp>

#include "core/error.hpp"
#include "core/version.hpp"

namespace astral::auth {

namespace {

using nlohmann::json;

constexpr std::chrono::seconds kSlowDownPenalty{5}; // RFC 8628 §3.2

[[noreturn]] void throwProtocol(const std::string& detail) {
    throw core::AstralError(core::Errc::ProtocolIncompatible, detail);
}

// Parses the error envelope {"error":{"code": ...}} of a non-2xx response.
std::string errorCodeOf(const client::HttpResponse& response) {
    try {
        const json body = json::parse(response.body);
        return body.at("error").value("code", std::string());
    } catch (const std::exception&) {
        return "";
    }
}

json parseObject(const client::HttpResponse& response, const std::string& what) {
    try {
        return json::parse(response.body);
    } catch (const std::exception&) {
        throwProtocol(what + ": response is not valid JSON");
    }
}

std::string requireString(const json& body, const char* key, const std::string& what) {
    const auto it = body.find(key);
    if (it == body.end() || !it->is_string() || it->get<std::string>().empty()) {
        throwProtocol(what + ": missing field '" + key + "'");
    }
    return it->get<std::string>();
}

} // namespace

platform::LoginSession runDeviceFlow(const std::string& serverUrl, const HttpFn& http,
                                     const SleepFn& sleep, const OnUserCodeFn& onUserCode) {
    const ServerInfo server = discoverServer(serverUrl, http);
    platform::LoginSession session;
    session.serverUrl = server.baseUrl;
    session.serverId = server.serverId;
    session.apiBase = server.apiBase;
    const std::string api = server.origin() + server.apiBase;

    // 1. Create the authorization request.
    client::HttpRequest create;
    create.method = "POST";
    create.url = api + "/auth/device/authorizations";
    create.body = R"({"client_type":"cli"})";
    const client::HttpResponse created = http(create);
    if (created.status != 201) {
        throwProtocol("device authorization returned status " + std::to_string(created.status));
    }
    const json authz = parseObject(created, "device authorization");
    const std::string deviceCode = requireString(authz, "device_code", "device authorization");
    const std::string verificationUri =
        requireString(authz, "verification_uri_complete", "device authorization");
    const std::string userCode = requireString(authz, "user_code", "device authorization");
    std::chrono::seconds interval(authz.value("interval", 3));
    const std::chrono::seconds expiresAt(authz.value("expires_in", 600));

    // 2. Hand the approval to the human.
    if (onUserCode) {
        onUserCode(verificationUri, userCode);
    }

    // 3. Poll. Elapsed time is accumulated from the (injectable) sleeps so
    // tests stay deterministic; the deadline is re-checked after each wait so
    // an expired device code is never polled again.
    std::chrono::milliseconds elapsed{0};
    const std::chrono::milliseconds deadline = expiresAt;
    for (;;) {
        sleep(interval);
        elapsed += interval;
        if (elapsed >= deadline) {
            throw core::AstralError(core::Errc::Timeout,
                                    "device code expired before approval (expires_in " +
                                        std::to_string(expiresAt.count()) + "s)");
        }

        client::HttpRequest request;
        request.method = "POST";
        request.url = api + "/auth/device/authorizations/" + deviceCode + "/token";
        request.body = "{}";
        const client::HttpResponse response = http(request);

        if (response.status == 200) {
            const json pair = parseObject(response, "token response");
            session.accessToken = requireString(pair, "access_token", "token response");
            session.refreshToken = requireString(pair, "refresh_token", "token response");
            session.principalId = pair.value("actor_id", std::string());
            return session;
        }
        const std::string code = errorCodeOf(response);
        if (response.status == 400 && code == "AUTHORIZATION_PENDING") {
            continue;
        }
        if (response.status == 400 && code == "SLOW_DOWN") {
            interval += kSlowDownPenalty;
            continue;
        }
        if (response.status == 401) {
            throw core::AstralError(core::Errc::AuthRequired,
                                    "authorization was denied or the device code expired");
        }
        throwProtocol("token polling returned status " + std::to_string(response.status) +
                      (code.empty() ? "" : " (" + code + ")"));
    }
}

} // namespace astral::auth
