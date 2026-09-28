#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#include "core/error.hpp"
#include "platform/credential_store.hpp"

namespace fs = std::filesystem;
using astral::platform::Credential;
using astral::platform::FileCredentialStore;

namespace {

fs::path makeTempFile() {
    static std::random_device device;
    std::stringstream name;
    name << "astral-creds-" << device() << ".json";
    const fs::path dir = fs::temp_directory_path() / name.str();
    fs::create_directories(dir);
    return dir / "credentials.json";
}

Credential sample(std::string token) {
    return Credential{std::move(token), "refresh-1", "user_01"};
}

} // namespace

TEST_CASE("credentials roundtrip through the file") {
    FileCredentialStore store(makeTempFile());
    store.save("srv_01", sample("access-A"));

    const auto loaded = store.load("srv_01");
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->accessToken == "access-A");
    REQUIRE(loaded->refreshToken == "refresh-1");
    REQUIRE(loaded->principalId == "user_01");
}

TEST_CASE("servers are stored independently in one file") {
    FileCredentialStore store(makeTempFile());
    store.save("srv_01", sample("access-A"));
    store.save("srv_02", sample("access-B"));

    REQUIRE(store.load("srv_01")->accessToken == "access-A");
    REQUIRE(store.load("srv_02")->accessToken == "access-B");
    REQUIRE_FALSE(store.load("srv_03").has_value());

    const auto servers = store.list();
    REQUIRE(servers.size() == 2);
}

TEST_CASE("a fresh store is empty and loadable") {
    FileCredentialStore store(makeTempFile());
    REQUIRE(store.list().empty());
    REQUIRE_FALSE(store.load("srv_01").has_value());
}

TEST_CASE("erase removes only the target server") {
    FileCredentialStore store(makeTempFile());
    store.save("srv_01", sample("access-A"));
    store.save("srv_02", sample("access-B"));

    store.erase("srv_01");
    REQUIRE_FALSE(store.load("srv_01").has_value());
    REQUIRE(store.load("srv_02")->accessToken == "access-B");
    REQUIRE(store.list().size() == 1);

    store.erase("srv_02");
    REQUIRE(store.list().empty());
}

TEST_CASE("malformed file fails load with CREDENTIAL_STORE_ERROR") {
    const fs::path file = makeTempFile();
    {
        std::ofstream output(file);
        output << "{ not json at all";
    }

    FileCredentialStore store(file);
    try {
        (void)store.load("srv_01");
        FAIL("expected an error");
    } catch (const astral::core::AstralError& error) {
        REQUIRE(error.code() == astral::core::Errc::CredentialStoreError);
    }
}

TEST_CASE("save rebuilds a malformed file, so login repairs it") {
    const fs::path file = makeTempFile();
    {
        std::ofstream output(file);
        output << "{ broken";
    }

    FileCredentialStore store(file);
    store.save("srv_01", sample("access-A"));
    REQUIRE(store.load("srv_01")->accessToken == "access-A");
    REQUIRE(store.list().size() == 1);
}

TEST_CASE("unknown file version is treated as malformed") {
    const fs::path file = makeTempFile();
    {
        std::ofstream output(file);
        output << R"({"version": 99, "servers": {}})";
    }

    FileCredentialStore store(file);
    REQUIRE_THROWS_AS(store.list(), astral::core::AstralError);
    store.save("srv_01", sample("access-A"));
    REQUIRE(store.load("srv_01").has_value());
}

#ifndef _WIN32
// Windows 的 NTFS 没有 POSIX 位掩码语义（MSVC status 对普通可写文件报 0777），
// 0600 断言只在 POSIX 上有意义；Windows 上由用户 profile 目录 ACL 保护。
TEST_CASE("written file is owner-only on POSIX") {
    const fs::path file = makeTempFile();
    FileCredentialStore store(file);
    store.save("srv_01", sample("access-A"));

    std::error_code ec;
    const fs::perms perms = fs::status(file, ec).permissions();
    const fs::perms allowed = fs::perms::owner_read | fs::perms::owner_write;
    REQUIRE((perms & fs::perms::mask) == allowed);
}
#endif

TEST_CASE("tokens are stored as plain JSON fields") {
    const fs::path file = makeTempFile();
    FileCredentialStore store(file);
    store.save("srv_01", sample("access-A"));

    std::ifstream input(file);
    // rdbuf 拷贝而非 istreambuf_iterator：gcc13 在 -O3 下对后者报
    // -Wnull-dereference 误报（streambuf gptr/egptr），/WX 会拒掉整次构建。
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string content = buffer.str();
    REQUIRE(content.find("\"access_token\": \"access-A\"") != std::string::npos);
    REQUIRE(content.find("\"srv_01\"") != std::string::npos);
}

TEST_CASE("login sessions round-trip in their own slot (D12)") {
    const fs::path file = makeTempFile();
    FileCredentialStore store(file);

    // Server-keyed credentials and URL-keyed sessions are independent slots.
    store.save("srv_01", sample("access-A"));

    astral::platform::LoginSession session;
    session.serverUrl = "https://s.example.com";
    session.serverId = "srv_01";
    session.apiBase = "/api/v1";
    session.accessToken = "at_x";
    session.refreshToken = "rt_x";
    session.principalId = "usr_01";
    store.saveSession(session.serverUrl, session);

    const auto loaded = store.loadSession("https://s.example.com");
    REQUIRE(loaded.has_value());
    CHECK(loaded->serverUrl == "https://s.example.com");
    CHECK(loaded->serverId == "srv_01");
    CHECK(loaded->apiBase == "/api/v1");
    CHECK(loaded->accessToken == "at_x");
    CHECK(loaded->refreshToken == "rt_x");
    CHECK(loaded->principalId == "usr_01");

    REQUIRE(store.load("srv_01").has_value()); // untouched by session writes
    CHECK(store.listSessions() == std::vector<std::string>{"https://s.example.com"});

    // Rotation overwrites in place.
    session.refreshToken = "rt_2";
    store.saveSession(session.serverUrl, session);
    CHECK(store.loadSession("https://s.example.com")->refreshToken == "rt_2");

    store.eraseSession("https://s.example.com");
    CHECK(!store.loadSession("https://s.example.com").has_value());
    CHECK(store.listSessions().empty());
}

TEST_CASE("missing session slot reads as logged out") {
    const fs::path file = makeTempFile();
    FileCredentialStore store(file);
    CHECK(!store.loadSession("https://s.example.com").has_value());
    CHECK(store.listSessions().empty());
}
