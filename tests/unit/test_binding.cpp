#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#include "core/error.hpp"
#include "workspace/binding.hpp"

namespace fs = std::filesystem;
using astral::workspace::Binding;
using astral::workspace::findBindingDir;
using astral::workspace::readBinding;
using astral::workspace::sameTarget;
using astral::workspace::writeBinding;

namespace {

fs::path makeTempDir() {
    static std::random_device device;
    std::stringstream name;
    name << "astral-test-" << device();
    const fs::path dir = fs::temp_directory_path() / name.str();
    fs::create_directories(dir);
    return dir;
}

Binding sampleBinding() {
    return Binding{1,       "srv_01",           "https://astral.example.com",
                   "ws_01", "astral-modulator", "astral-modulator"};
}

} // namespace

TEST_CASE("binding write/read roundtrip") {
    const fs::path dir = makeTempDir();
    const Binding original = sampleBinding();
    writeBinding(dir, original);

    const auto loaded = readBinding(dir);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->serverId == original.serverId);
    REQUIRE(loaded->serverUrl == original.serverUrl);
    REQUIRE(loaded->workspaceId == original.workspaceId);
    REQUIRE(loaded->workspaceName == original.workspaceName);
    REQUIRE(loaded->workspaceSlug == original.workspaceSlug);
}

TEST_CASE("binding is found by walking upward from nested directories") {
    const fs::path root = makeTempDir();
    writeBinding(root, sampleBinding());

    const fs::path nested = root / "a" / "b" / "c";
    fs::create_directories(nested);

    const auto found = findBindingDir(nested);
    REQUIRE(found.has_value());
    REQUIRE(fs::equivalent(*found, root));

    const fs::path outside = makeTempDir();
    REQUIRE_FALSE(findBindingDir(outside).has_value());
}

TEST_CASE("missing binding file reads as nullopt") {
    REQUIRE_FALSE(readBinding(makeTempDir()).has_value());
}

TEST_CASE("malformed binding fails with LOCAL_WORKSPACE_ERROR") {
    const fs::path dir = makeTempDir();
    fs::create_directories(dir / ".astral");
    {
        std::ofstream output(dir / ".astral" / "config.json");
        output << "{ not json";
    }
    REQUIRE_THROWS_AS(readBinding(dir), astral::core::AstralError);
    try {
        readBinding(dir);
    } catch (const astral::core::AstralError& error) {
        REQUIRE(error.code() == astral::core::Errc::LocalWorkspaceError);
    }
}

TEST_CASE("binding with unknown version is rejected") {
    const fs::path dir = makeTempDir();
    fs::create_directories(dir / ".astral");
    {
        std::ofstream output(dir / ".astral" / "config.json");
        output << R"({"version": 99, "server": {"id": "s", "url": "u"},)"
               << R"( "workspace": {"id": "w", "name": "n", "slug": "n"}})";
    }
    try {
        readBinding(dir);
        FAIL("expected an error");
    } catch (const astral::core::AstralError& error) {
        REQUIRE(error.code() == astral::core::Errc::LocalWorkspaceError);
    }
}

TEST_CASE("sameTarget keys on ids, not display fields") {
    const Binding left = sampleBinding();
    Binding renamed = left;
    renamed.workspaceName = "renamed";
    renamed.serverUrl = "https://mirror.example.com";
    REQUIRE(sameTarget(left, renamed));

    Binding otherWorkspace = left;
    otherWorkspace.workspaceId = "ws_02";
    REQUIRE_FALSE(sameTarget(left, otherWorkspace));
}

TEST_CASE("writeBinding overwrites atomically and leaves no temp files") {
    const fs::path dir = makeTempDir();
    writeBinding(dir, sampleBinding());

    Binding updated = sampleBinding();
    updated.workspaceId = "ws_02";
    writeBinding(dir, updated);

    const auto loaded = readBinding(dir);
    REQUIRE(loaded->workspaceId == "ws_02");

    int entries = 0;
    for (const auto& entry : fs::directory_iterator(dir / ".astral")) {
        (void)entry;
        ++entries;
    }
    REQUIRE(entries == 1);
}
