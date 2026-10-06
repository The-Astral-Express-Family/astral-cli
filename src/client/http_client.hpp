#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
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

// Percent-encodes a single query component (spaces, '&', '=', unicode...);
// unreserved characters stay literal. Building query strings for server
// parameters of free-form shape (regex/fuzzy) is mandatory, not cosmetic.
std::string urlEncode(const std::string& value);

// Receives body chunks as they arrive; returning false aborts the transfer.
using ChunkSink = std::function<bool(std::string_view chunk)>;

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
        // 资产下载（GitHub Release 302 跳转到 objects.githubusercontent.com）需要
        // 跟随重定向；默认开启，API 查询同样无害。
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

    // Streaming variant for long-lived endpoints (SSE). Every body chunk is
    // passed to onChunk as it arrives; a false return aborts the transfer and
    // the response is returned with its status (clean client stop). There is
    // deliberately no total-response timeout; the connect timeout still
    // applies and a stall detector (server keepalives are 15s) aborts dead
    // connections as TIMEOUT. Non-200 bodies are buffered into response.body
    // instead of streamed to onChunk, so error envelopes stay readable.
    HttpResponse sendStreaming(const HttpRequest& request, const ChunkSink& onChunk);

    // 流式下载响应体到文件（update 资产下载用，不占内存）。HTTP >=400 时按
    // 现有错误路径抛 AstralError(NETWORK_ERROR) 并删除半截文件；目标路径
    // 不可写时抛 LOCAL_WORKSPACE_ERROR（发网络请求之前）。
    HttpResponse getToFile(const std::string& url, const std::filesystem::path& destination);

    // 运行时可调的选项副本（如 GithubReleaseClient 注入 Accept 头）。
    Options& options() { return options_; }
    const Options& options() const { return options_; }

private:
    Options options_;
};

} // namespace astral::client
