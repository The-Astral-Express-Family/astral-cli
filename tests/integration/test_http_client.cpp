#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#include "client/http_client.hpp"
#include "core/error.hpp"

using astral::client::HttpClient;
using astral::client::HttpResponse;

namespace {

std::string testServer() {
    const char* server = std::getenv("ASTRAL_TEST_SERVER");
    return server == nullptr ? std::string() : std::string(server);
}

// Download target for getToFile: the live Astral test server when configured,
// otherwise the public GitHub API (the real update-flow download host).
std::string downloadUrl() {
    const std::string server = testServer();
    return server.empty() ? std::string("https://api.github.com/") : server + "/.well-known/astral";
}

// URL that answers with an HTTP error status on both target kinds.
std::string missingUrl() {
    const std::string server = testServer();
    return server.empty()
               ? std::string("https://api.github.com/definitely-not-a-real-path-for-tests")
               : server + "/definitely-not-a-real-path-for-tests";
}

void removeQuietly(const std::filesystem::path& path) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    // GCC 13 -O3 误报规避：istreambuf_iterator -> seekg/tellg/read。
    std::string bytes;
    file.seekg(0, std::ios::end);
    const auto size = file.tellg();
    if (size > 0) {
        bytes.resize(static_cast<std::size_t>(size));
        file.seekg(0, std::ios::beg);
        file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        bytes.resize(static_cast<std::size_t>(file.gcount()));
    }
    return bytes;
}

} // namespace

TEST_CASE("GET /.well-known/astral against a live server", "[integration][net]") {
    const std::string server = testServer();
    if (server.empty()) {
        SKIP("ASTRAL_TEST_SERVER is not set");
    }

    HttpClient client;
    const auto response = client.get(server + "/.well-known/astral");
    REQUIRE(response.status >= 200);
    REQUIRE(response.status < 500);
}

TEST_CASE("connection failures map to NETWORK_ERROR", "[integration][net]") {
    HttpClient::Options options;
    options.connectTimeout = std::chrono::seconds(1);
    options.requestTimeout = std::chrono::seconds(1);
    HttpClient client(options);

    bool threw = false;
    try {
        (void)client.get("http://127.0.0.1:1/"); // port 1 refuses everywhere
    } catch (const astral::core::AstralError& error) {
        threw = true;
        REQUIRE(error.code() == astral::core::Errc::NetworkError);
    }
    REQUIRE(threw);
}

TEST_CASE("getToFile writes the body to disk and matches get()", "[integration][net]") {
    const std::string server = testServer();
    const std::string url = downloadUrl();

    HttpClient client;
    // Probe reachability so offline environments skip instead of failing:
    // without ASTRAL_TEST_SERVER the fallback is the public GitHub API.
    HttpResponse response;
    bool reachable = true;
    try {
        response = client.get(url);
    } catch (const astral::core::AstralError&) {
        reachable = false;
    }
    if (!reachable) {
        if (server.empty()) {
            SKIP("api.github.com unreachable and ASTRAL_TEST_SERVER is not set");
        }
        FAIL("request against ASTRAL_TEST_SERVER failed");
    }

    const auto destination =
        std::filesystem::temp_directory_path() / "astral-gettofile-integration.bin";
    removeQuietly(destination);

    const auto streamed = client.getToFile(url, destination);

    REQUIRE(streamed.status == response.status);
    REQUIRE(std::filesystem::file_size(destination) > 0);
    REQUIRE(readFile(destination) == response.body);
    removeQuietly(destination);
}

TEST_CASE("getToFile throws and deletes the partial file on HTTP error statuses",
          "[integration][net]") {
    const std::string server = testServer();
    const std::string url = missingUrl();

    HttpClient client;
    // Probe reachability so offline environments skip instead of failing.
    bool reachable = true;
    long status = 0;
    try {
        status = client.get(url).status;
    } catch (const astral::core::AstralError&) {
        reachable = false;
    }
    if (!reachable) {
        if (server.empty()) {
            SKIP("api.github.com unreachable and ASTRAL_TEST_SERVER is not set");
        }
        FAIL("request against ASTRAL_TEST_SERVER failed");
    }
    REQUIRE(status >= 400);

    const auto destination =
        std::filesystem::temp_directory_path() / "astral-gettofile-http-error.bin";
    removeQuietly(destination);

    bool threw = false;
    try {
        (void)client.getToFile(url, destination);
    } catch (const astral::core::AstralError& error) {
        threw = true;
        REQUIRE(error.code() == astral::core::Errc::NetworkError);
    }
    REQUIRE(threw);
    REQUIRE_FALSE(std::filesystem::exists(destination));
    removeQuietly(destination);
}
