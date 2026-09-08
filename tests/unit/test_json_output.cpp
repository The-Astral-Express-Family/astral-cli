#include <catch2/catch_test_macros.hpp>

#include <sstream>

#include <nlohmann/json.hpp>

#include "output/json_output.hpp"

using astral::output::errorEnvelope;
using astral::output::printJson;
using astral::output::printJsonError;

TEST_CASE("printJson emits exactly one object per line") {
    std::ostringstream out;
    printJson(out, nlohmann::json{{"ok", true}});
    const std::string text = out.str();

    REQUIRE(text.back() == '\n');
    REQUIRE(text.find('\n', 0) == text.size() - 1);

    const auto parsed = nlohmann::json::parse(text);
    REQUIRE(parsed.at("ok").get<bool>());
}

TEST_CASE("error envelope carries stable code and message") {
    const auto envelope = errorEnvelope("AUTH_REQUIRED", "no credential");
    REQUIRE(envelope.at("error").at("code") == "AUTH_REQUIRED");
    REQUIRE(envelope.at("error").at("message") == "no credential");

    std::ostringstream out;
    printJsonError(out, "AUTH_REQUIRED", "no credential");
    const auto parsed = nlohmann::json::parse(out.str());
    REQUIRE(parsed == envelope);
}
