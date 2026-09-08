#include <catch2/catch_test_macros.hpp>
#include <catch2/skip.hpp>

#include <cstdlib>
#include <string>

#include "client/http_client.hpp"

using astral::client::HttpClient;

namespace {

std::string testServer() {
    const char* server = std::getenv("ASTRAL_TEST_SERVER");
    return server == nullptr ? std::string() : std::string(server);
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
