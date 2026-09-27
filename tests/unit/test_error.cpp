#include <catch2/catch_test_macros.hpp>

#include "core/error.hpp"

using astral::core::AstralError;
using astral::core::Errc;

TEST_CASE("AstralError maps Errc to stable code strings and exit codes") {
    SECTION("UpdateIntegrity reports UPDATE_INTEGRITY and exit code 10") {
        AstralError error(Errc::UpdateIntegrity, "supply chain check failed");
        REQUIRE(error.code() == Errc::UpdateIntegrity);
        REQUIRE(error.codeString() == "UPDATE_INTEGRITY");
        REQUIRE(error.exitCode() == 10);
    }

    SECTION("existing mappings stay stable") {
        REQUIRE(AstralError(Errc::AuthRequired, "auth").codeString() == "AUTH_REQUIRED");
        REQUIRE(AstralError(Errc::AuthRequired, "auth").exitCode() == 3);
        REQUIRE(AstralError(Errc::NetworkError, "net").codeString() == "NETWORK_ERROR");
        REQUIRE(AstralError(Errc::NetworkError, "net").exitCode() == 6);
        REQUIRE(AstralError(Errc::Internal, "internal").exitCode() == 1);
    }
}
