#include "client/http_client.hpp"

#include <curl/curl.h>

#include <mutex>

#include "core/error.hpp"
#include "core/version.hpp"

namespace astral::client {

namespace {

std::once_flag gCurlInitOnce;

void ensureCurlGlobalInit() {
    std::call_once(gCurlInitOnce, [] {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        // Intentionally never call curl_global_cleanup: the process may still
        // hold handles at exit and teardown order is not guaranteed.
    });
}

size_t appendToBody(char* data, size_t size, size_t count, void* userData) {
    auto* body = static_cast<std::string*>(userData);
    body->append(data, size * count);
    return size * count;
}

size_t collectHeader(char* data, size_t size, size_t count, void* userData) {
    const std::string_view line(data, size * count);
    auto* headers = static_cast<std::vector<std::pair<std::string, std::string>>*>(userData);
    const auto colon = line.find(':');
    if (colon != std::string_view::npos) {
        auto trim = [](std::string_view text) {
            while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
                text.remove_prefix(1);
            }
            while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                                     text.back() == '\r' || text.back() == '\n')) {
                text.remove_suffix(1);
            }
            return std::string(text);
        };
        headers->emplace_back(trim(line.substr(0, colon)), trim(line.substr(colon + 1)));
    }
    return size * count;
}

[[noreturn]] void throwCurlFailure(CURLcode code, const std::string& url) {
    const std::string detail = curl_easy_strerror(code);
    if (code == CURLE_OPERATION_TIMEDOUT) {
        throw core::AstralError(core::Errc::Timeout, "request timed out: " + url);
    }
    throw core::AstralError(core::Errc::NetworkError, detail + " (" + url + ")");
}

} // namespace

std::optional<std::string> HttpResponse::header(const std::string& name) const {
    for (const auto& [key, value] : headers) {
        if (key == name) {
            return value;
        }
    }
    return std::nullopt;
}

HttpClient::HttpClient() : HttpClient(Options{}) {}

HttpClient::HttpClient(Options options) : options_(std::move(options)) {
    ensureCurlGlobalInit();
    if (options_.userAgent.empty()) {
        options_.userAgent = std::string("astral-cli/") + core::kProjectVersion;
    }
}

HttpClient::~HttpClient() = default;

HttpResponse HttpClient::send(const HttpRequest& request) {
    if (request.url.find("://") == std::string::npos) {
        throw core::AstralError(core::Errc::NetworkError,
                                "invalid URL (missing scheme): " + request.url);
    }

    CURL* handle = curl_easy_init();
    if (handle == nullptr) {
        throw core::AstralError(core::Errc::Internal, "curl_easy_init failed");
    }

    HttpResponse response;
    struct curl_slist* headerList = nullptr;
    auto cleanup = [&handle, &headerList] {
        if (headerList != nullptr) {
            curl_slist_free_all(headerList);
        }
        curl_easy_cleanup(handle);
    };

    headerList = curl_slist_append(headerList, "Accept: application/json");
    for (const auto& [key, value] : request.headers) {
        headerList = curl_slist_append(headerList, (key + ": " + value).c_str());
    }
    if (request.bearerToken) {
        headerList = curl_slist_append(headerList,
                                       ("Authorization: Bearer " + *request.bearerToken).c_str());
    }

    curl_easy_setopt(handle, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_USERAGENT, options_.userAgent.c_str());
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS,
                     static_cast<long>(options_.connectTimeout.count()));
    curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS,
                     static_cast<long>(options_.requestTimeout.count()));
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, options_.verifyTls ? 1L : 0L);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, options_.verifyTls ? 2L : 0L);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, appendToBody);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, collectHeader);
    curl_easy_setopt(handle, CURLOPT_HEADERDATA, &response.headers);
    if (!request.body.empty() || request.method == "POST" || request.method == "PUT" ||
        request.method == "PATCH" || request.method == "DELETE") {
        curl_easy_setopt(handle, CURLOPT_COPYPOSTFIELDS, request.body.c_str());
    }
    if (request.method != "GET" && request.method != "POST") {
        curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, request.method.c_str());
    }
    if (headerList != nullptr) {
        curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headerList);
    }

    const CURLcode result = curl_easy_perform(handle);
    if (result != CURLE_OK) {
        cleanup();
        throwCurlFailure(result, request.url);
    }

    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response.status);
    cleanup();
    return response;
}

HttpResponse HttpClient::get(const std::string& url, std::optional<std::string> bearer) {
    HttpRequest request;
    request.method = "GET";
    request.url = url;
    request.bearerToken = std::move(bearer);
    return send(request);
}

HttpResponse HttpClient::postJson(const std::string& url, const std::string& jsonBody,
                                  std::optional<std::string> bearer) {
    HttpRequest request;
    request.method = "POST";
    request.url = url;
    request.body = jsonBody;
    request.bearerToken = std::move(bearer);
    request.headers.emplace_back("Content-Type", "application/json");
    return send(request);
}

} // namespace astral::client
