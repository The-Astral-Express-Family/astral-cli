// astral init 命令的 runApp 级回归（E2E r40 发现）：
// 简写 <server>/<workspace> 的 well-known discovery 必须打在剥掉工作区
// 段之后的服务器 URL 上（ARCHITECTURE.md 9.3）；此前误用 fullUrl，同源
// 部署会把 SPA HTML 当 JSON 解析成 PROTOCOL_INCOMPATIBLE。
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "support/api_fixture.hpp"

namespace {

using json = nlohmann::json;
using astral_test::ApiFixture;
using astral_test::runApp;
using astral_test::RunResult;

const json kWorkspace = json{{"id", "ws_1"}, {"name", "my-repo"}, {"slug", "my-repo"}};

TEST_CASE("init shorthand discovers well-known on the split server URL") {
    ApiFixture fx;
    fx.installSession();
    fx.fake().route("/workspaces?name=my-repo", 200, json{{"items", json::array({kWorkspace})}});
    fx.fake().route("/workspaces/ws_1", 200, kWorkspace);

    const RunResult result = runApp({"astral", "init", "https://api.test/my-repo"});
    REQUIRE(result.exitCode == 0);
    REQUIRE_FALSE(fx.fake().requests.empty());
    // 相等断言（而非子串）：FakeApi 按 needle 子串路由，会吞掉错误后缀，
    // 只有相等断言能证明 discovery URL 不含工作区段。
    REQUIRE(fx.fake().requests.front().url == "https://api.test/.well-known/astral");
}

TEST_CASE("init with explicit --workspace discovers on the whole input") {
    ApiFixture fx;
    fx.installSession();
    fx.fake().route("/workspaces?name=other", 200, json{{"items", json::array({kWorkspace})}});
    fx.fake().route("/workspaces/ws_1", 200, kWorkspace);

    const RunResult result = runApp({"astral", "init", "https://api.test", "--workspace", "other"});
    REQUIRE(result.exitCode == 0);
    REQUIRE_FALSE(fx.fake().requests.empty());
    // 无拆分点（显式 --workspace）时回退 fullUrl，同样不得崩在空 urlAfterSplit。
    REQUIRE(fx.fake().requests.front().url == "https://api.test/.well-known/astral");
}

} // namespace
