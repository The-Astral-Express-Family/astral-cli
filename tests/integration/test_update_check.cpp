#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_tostring.hpp>

#include <sstream>
#include <string>
#include <vector>

#include "app/app.hpp"
#include "client/http_client.hpp"

// [integration][net] 依赖真实 api.github.com（匿名限速 60 req/h，本测试 1-2 个请求）。
// 离线环境 SKIP：探测失败不算测试失败。

namespace {

bool githubReachable() {
    try {
        astral::client::HttpClient http;
        const auto response = http.get("https://api.github.com/");
        return response.status < 500;
    } catch (...) {
        return false;
    }
}

struct RunResult {
    int exitCode = 0;
    std::string out;
    std::string err;
};

RunResult run(std::vector<std::string> args) {
    std::ostringstream out;
    std::ostringstream err;
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    const int code =
        astral::app::runApp(static_cast<int>(argv.size()), argv.data(), out, err);
    return {code, out.str(), err.str()};
}

} // namespace

TEST_CASE("update --check hits the real GitHub API and reports latest", "[integration][net]") {
    if (!githubReachable()) {
        SKIP("offline: no network access to api.github.com");
    }

    const auto result = run({"astral", "update", "--check", "--json"});
    INFO("stdout: " << result.out);
    INFO("stderr: " << result.err);

    // 两种合法终态：
    //  - 仓库已有 release：exit 0 且 JSON 含 "latest" 字段；
    //  - 仓库尚无任何 release（首个 tag 打出前的窗口期）：exit 4 且错误消息明确。
    const bool hasRelease = result.exitCode == 0;
    if (hasRelease) {
        REQUIRE(result.out.find("\"latest\"") != std::string::npos);
    } else {
        REQUIRE(result.exitCode == 4);
        REQUIRE(result.out.find("no releases published") != std::string::npos);
    }
}
