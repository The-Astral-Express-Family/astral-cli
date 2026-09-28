#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <system_error>

#include "client/http_client.hpp"
#include "core/error.hpp"

using astral::client::HttpClient;

TEST_CASE("HttpClient::Options follows redirects by default with no extra headers") {
    const HttpClient::Options options;
    REQUIRE(options.followRedirects);
    REQUIRE(options.headers.empty());
}

TEST_CASE("getToFile leaves no partial file when the request fails before any data") {
    HttpClient client;
    const auto destination =
        std::filesystem::temp_directory_path() / "astral-gettofile-transport-failed.bin";
    std::error_code ignored;
    std::filesystem::remove(destination, ignored);

    bool threw = false;
    try {
        // Missing "://" trips the same validation send() performs; the file
        // must not survive as an empty leftover.
        (void)client.getToFile("example.invalid/no-scheme", destination);
    } catch (const astral::core::AstralError& error) {
        threw = true;
        REQUIRE(error.code() == astral::core::Errc::NetworkError);
    }
    REQUIRE(threw);
    REQUIRE_FALSE(std::filesystem::exists(destination));
    std::filesystem::remove(destination, ignored);
}

TEST_CASE("getToFile fails with LOCAL_WORKSPACE_ERROR when the destination cannot be opened") {
    HttpClient client;
    const auto destination =
        std::filesystem::temp_directory_path() / "astral-missing-download-dir" / "asset.bin";

    bool threw = false;
    try {
        (void)client.getToFile("https://localhost/asset.bin", destination);
    } catch (const astral::core::AstralError& error) {
        threw = true;
        REQUIRE(error.code() == astral::core::Errc::LocalWorkspaceError);
    }
    REQUIRE(threw);
}
