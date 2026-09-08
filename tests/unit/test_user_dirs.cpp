#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "platform/user_dirs.hpp"

namespace fs = std::filesystem;
using astral::platform::astralHomeFor;
using astral::platform::cacheDirFor;

TEST_CASE("all user data lives under ~/.astral-cli on every platform") {
    REQUIRE(astralHomeFor("/home/u") == fs::path("/home/u/.astral-cli"));
    REQUIRE(astralHomeFor("C:/Users/u") == fs::path("C:/Users/u/.astral-cli"));
    REQUIRE(astralHomeFor("/Users/u") == fs::path("/Users/u/.astral-cli"));
}

TEST_CASE("cache lives inside the astral home") {
    REQUIRE(cacheDirFor("/home/u") == fs::path("/home/u/.astral-cli/cache"));
    REQUIRE(cacheDirFor("/home/u").parent_path() == astralHomeFor("/home/u"));
}

TEST_CASE("configDir is an alias of astralHome") {
    REQUIRE(astral::platform::configDir() == astral::platform::astralHome());
    REQUIRE(astral::platform::cacheDir() == astral::platform::astralHome() / "cache");
}
