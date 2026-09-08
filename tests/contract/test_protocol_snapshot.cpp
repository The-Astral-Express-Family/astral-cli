#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "core/version.hpp"

// The snapshot under test mirrors the discovery document shape from
// ARCHITECTURE.md section 5; when astral-modulator publishes new snapshots,
// the required-field assertions here are the first thing to reconcile.
TEST_CASE("v1 discovery snapshot keeps its promised fields") {
    std::ifstream input(std::string(ASTRAL_PROTOCOL_DIR) + "/snapshots/v1/well-known.json");
    REQUIRE(input.good());

    const auto doc = nlohmann::json::parse(input);
    REQUIRE(doc.at("server_id").is_string());
    REQUIRE(doc.at("canonical_url").is_string());
    REQUIRE(doc.at("api_base").is_string());
    REQUIRE(doc.at("protocol_version").is_number_integer());
    REQUIRE(doc.at("min_cli_protocol_version").is_number_integer());
    REQUIRE(doc.at("auth").at("device_login").is_boolean());
}

TEST_CASE("this CLI speaks the snapshot protocol version") {
    std::ifstream input(std::string(ASTRAL_PROTOCOL_DIR) + "/snapshots/v1/well-known.json");
    REQUIRE(input.good());

    const auto doc = nlohmann::json::parse(input);
    REQUIRE(astral::core::kProtocolVersion >= doc.at("min_cli_protocol_version").get<int>());
}
