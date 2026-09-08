#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace astral::client {

struct HttpRequest {
    std::string method = "GET";
    std::string url;
    std::optional<std::string> bearerToken;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
};

struct HttpResponse {
    long status = 0;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;

    std::optional<std::string> header(const std::string& name) const;
};

// Thin libcurl wrapper. Bounded behavior only: no global retry loop here -
// retry/backoff policy lives one layer up once wired to the real protocol
// (ARCHITECTURE.md section 13).
class HttpClient {
public:
    struct Options {
        std::chrono::milliseconds connectTimeout{std::chrono::seconds(10)};
        std::chrono::milliseconds requestTimeout{std::chrono::seconds(30)};
        std::string userAgent;
        // TLS verification is always on; a per-request escape hatch will be a
        // one-shot dev flag, never a persisted setting.
        bool verifyTls = true;
    };

    // Two constructors instead of a defaulted `Options = {}` argument:
    // GCC 16 rejects `{}`/`Options()` default arguments for a nested aggregate
    // with NSDMIs (complete-class context parsing).
    HttpClient();
    explicit HttpClient(Options options);
    ~HttpClient();

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    // Throws AstralError(TIMEOUT) / AstralError(NETWORK_ERROR) on transport
    // failures; HTTP error statuses are returned as-is for the caller to map.
    HttpResponse send(const HttpRequest& request);

    HttpResponse get(const std::string& url, std::optional<std::string> bearer = std::nullopt);
    HttpResponse postJson(const std::string& url, const std::string& jsonBody,
                          std::optional<std::string> bearer = std::nullopt);

private:
    Options options_;
};

} // namespace astral::client
