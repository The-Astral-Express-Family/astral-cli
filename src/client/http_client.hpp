#pragma once

#include <chrono>
#include <filesystem>
#include <map>
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
        // Follow 3xx Location chains, capped at 5 hops: release asset
        // downloads redirect off api.github.com. Off = raw statuses returned.
        bool followRedirects = true;
        // Extra headers sent with every request (sorted by key). Sent in
        // addition to per-request headers; callers must not set User-Agent
        // or Host here (curl sends duplicate keys verbatim).
        std::map<std::string, std::string> headers;
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

    // Streams the body of a GET to `destination` without buffering it in
    // memory (update asset downloads). Transport errors throw exactly like
    // get(); a final status >= 400 throws AstralError(NETWORK_ERROR) after
    // removing the partial file; an unopenable destination throws
    // AstralError(LOCAL_WORKSPACE_ERROR) without touching the network.
    HttpResponse getToFile(const std::string& url, const std::filesystem::path& destination);

    // 运行时可调的选项副本（如 GithubReleaseClient 注入 Accept 头）。
    Options& options() { return options_; }
    const Options& options() const { return options_; }

private:
    // Shared curl setup/teardown for send() and getToFile(); `bodySink` is the
    // pointer handed to `writeBody` (response.body or an output file stream).
    void perform(const HttpRequest& request, std::size_t (*writeBody)(char*, std::size_t,
                                                                      std::size_t, void*),
                 void* bodySink, HttpResponse& response);

    Options options_;
};

} // namespace astral::client
