#pragma once

// Shared runApp-level test scaffolding: scripted HTTP transport, isolated
// ASTRAL_HOME / working directory, session fixture. Tests include this header
// and stay free of env/cwd/network side effects.
//
// 备注（本目录命名）：此 support 头只服务 tests/unit 的 runApp 级用例，不进
// astral_core；放在 tests/unit/support/ 与生产 src/ 的分层保持可见。

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "app/app.hpp"
#include "auth/session.hpp"
#include "client/http_client.hpp"
#include "core/exit_codes.hpp"
#include "platform/credential_store.hpp"
#include "workspace/binding.hpp"

namespace astral_test {

inline void setEnv(const std::string& name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name.c_str(), value.c_str());
#else
    setenv(name.c_str(), value.c_str(), 1);
#endif
}

inline void unsetEnv(const std::string& name) {
#ifdef _WIN32
    _putenv_s(name.c_str(), "");
#else
    unsetenv(name.c_str());
#endif
}

inline std::optional<std::string> readEnv(const std::string& name) {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    const char* value = std::getenv(name.c_str());
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
}

struct EnvGuard {
    EnvGuard(std::string name, const std::string& value) : name_(std::move(name)) {
        previous_ = readEnv(name_);
        setEnv(name_, value);
    }
    ~EnvGuard() {
        if (previous_) {
            setEnv(name_, *previous_);
        } else {
            unsetEnv(name_);
        }
    }
    EnvGuard(const EnvGuard&) = delete;
    EnvGuard& operator=(const EnvGuard&) = delete;

    std::string name_;
    std::optional<std::string> previous_;
};

struct CwdGuard {
    explicit CwdGuard(const std::filesystem::path& dir)
        : previous_(std::filesystem::current_path()) {
        std::filesystem::current_path(dir);
    }
    ~CwdGuard() {
        std::error_code ec;
        std::filesystem::current_path(previous_, ec);
    }
    CwdGuard(const CwdGuard&) = delete;
    CwdGuard& operator=(const CwdGuard&) = delete;

    std::filesystem::path previous_;
};

inline std::filesystem::path uniqueHome(const char* stem) {
    static int counter = 0;
    return std::filesystem::temp_directory_path() /
           (std::string(stem) + "-" + std::to_string(++counter));
}

// ---- scripted transport --------------------------------------------------
//
// Routes match by URL substring, first match wins; every route is consumed
// after `uses` hits so identical URLs can script ordered responses (e.g. the
// revision-read GET then the claim POST against /tasks/{id}).

struct ScriptedResponse {
    int status = 0;
    std::string body;
};

struct FakeApi {
    struct Route {
        std::string needle;
        ScriptedResponse response;
        int uses = 1;
    };

    std::vector<Route> routes;
    std::vector<astral::client::HttpRequest> requests;

    void route(const std::string& needle, int status, const nlohmann::json& body, int uses = 1) {
        routes.push_back(Route{needle, ScriptedResponse{status, body.dump()}, uses});
    }

    astral::client::HttpResponse operator()(const astral::client::HttpRequest& request) {
        requests.push_back(request);
        for (Route& candidate : routes) {
            if (candidate.uses > 0 && request.url.find(candidate.needle) != std::string::npos) {
                candidate.uses -= 1;
                astral::client::HttpResponse response;
                response.status = candidate.response.status;
                response.body = candidate.response.body;
                return response;
            }
        }
        FAIL("unexpected request URL: " << request.url);
        return astral::client::HttpResponse{};
    }
};

struct RunResult {
    int exitCode = -1;
    std::string out;
    std::string err;
};

inline RunResult runApp(const std::vector<std::string>& words) {
    std::vector<std::string> owned = words;
    std::vector<char*> argv;
    for (const std::string& word : owned) {
        argv.push_back(const_cast<char*>(word.c_str()));
    }
    std::ostringstream out;
    std::ostringstream err;
    const int code = astral::app::runApp(static_cast<int>(argv.size()), argv.data(), out, err);
    return RunResult{code, out.str(), err.str()};
}

inline bool requestHasBearer(const astral::client::HttpRequest& request, const std::string& token) {
    // Bearer rides the dedicated field; the Authorization header is derived
    // by the real HTTP client (the fake bypasses it).
    return request.bearerToken.has_value() && *request.bearerToken == token;
}

inline std::string errorEnvelopeBody(const std::string& code, const std::string& message,
                                     bool retryable = false,
                                     const std::string& requestId = "req_1") {
    return nlohmann::json{{"error",
                           {{"code", code},
                            {"message", message},
                            {"retryable", retryable},
                            {"request_id", requestId}}}}
        .dump();
}

// Isolated home + workspace binding (ws_1 @ https://api.test) + ASTRAL_TOKEN
// auth + scripted transport with the well-known route pre-registered.
class ApiFixture {
public:
    ApiFixture()
        : homeValue_(uniqueHome("astral-test")), home_("ASTRAL_HOME", homeValue_.string()),
          token_("ASTRAL_TOKEN", "astral_testtoken") {
        std::filesystem::create_directories(workDir());
        // Bind the working directory to ws_1 so commands resolve their target
        // without flag/env gymnastics.
        astral::workspace::writeBinding(
            workDir(),
            astral::workspace::Binding{1, "srv_test", "https://api.test", "ws_1", "demo", "demo"});
        cwd_.emplace(workDir());
        fake_.route("/.well-known/astral", 200, wellKnown(), 1000);
        astral::auth::setCommandTransportForTests(
            [this](const astral::client::HttpRequest& request) { return fake_(request); });
    }

    ~ApiFixture() { astral::auth::setCommandTransportForTests(nullptr); }

    ApiFixture(const ApiFixture&) = delete;
    ApiFixture& operator=(const ApiFixture&) = delete;

    FakeApi& fake() { return fake_; }

    const std::filesystem::path& workDir() const { return workDirValue_; }

    // Drops ASTRAL_TOKEN and installs a refreshable human session; used by
    // the lazy-refresh tests.
    void installSession() {
        unsetEnv("ASTRAL_TOKEN");
        auto store = astral::platform::makeDefaultCredentialStore();
        astral::platform::LoginSession session;
        session.serverUrl = "https://api.test";
        session.serverId = "srv_test";
        session.apiBase = "/api/v1";
        session.accessToken = "at_old";
        session.refreshToken = "rt_old";
        session.principalId = "usr_1";
        store->saveSession(session.serverUrl, session);
    }

    static nlohmann::json wellKnown() {
        return nlohmann::json{{"server_id", "srv_test"},
                              {"canonical_url", "https://api.test"},
                              {"api_base", "/api/v1"},
                              {"protocol_version", 2},
                              {"min_cli_protocol_version", 2}};
    }

private:
    std::filesystem::path homeValue_;
    std::filesystem::path workDirValue_ = homeValue_ / "work";
    EnvGuard home_;
    EnvGuard token_;
    std::optional<CwdGuard> cwd_;
    FakeApi fake_;
};

} // namespace astral_test
