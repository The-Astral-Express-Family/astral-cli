// runListenLoop 的重连/续传语义 + event listen 命令面的本地行为。
// 流式尝试全部用脚本化 fake（喂 chunk、回状态），不经 libcurl。

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <chrono>
#include <deque>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "client/http_client.hpp"
#include "core/exit_codes.hpp"
#include "events/listen.hpp"
#include "support/api_fixture.hpp"

namespace {

using astral::client::ChunkSink;
using astral::client::HttpRequest;
using astral::client::HttpResponse;
using astral::events::ListenOptions;
using astral::events::runListenLoop;

// 记录每次连接的请求头 + 按剧本喂 chunk。剧本项耗尽后连接直接以 200 收尾
// （服务器关流），让循环自然走重连路径。
struct ScriptedStream {
    struct Episode {
        long status = 200;
        std::string chunks;          // 逐段喂给 sink 的原始字节
        bool transportError = false; // true = 抛 NETWORK_ERROR
    };

    std::deque<Episode> episodes;
    std::vector<HttpRequest> requests;
    std::vector<std::string> lastEventIds; // 每次连接观察到的 Last-Event-ID

    HttpResponse connect(const HttpRequest& request, const ChunkSink& sink) {
        requests.push_back(request);
        std::string lastEventId;
        for (const auto& [name, value] : request.headers) {
            if (name == "Last-Event-ID") {
                lastEventId = value;
            }
        }
        lastEventIds.push_back(lastEventId);

        REQUIRE(!episodes.empty());
        const Episode episode = episodes.front();
        episodes.pop_front();
        if (episode.transportError) {
            throw astral::core::AstralError(astral::core::Errc::NetworkError,
                                            "connection reset by peer");
        }
        HttpResponse response;
        response.status = episode.status;
        if (episode.status == 200) {
            // 按真实流式语义逐段喂入。
            for (std::size_t start = 0; start < episode.chunks.size(); start += 7) {
                if (!sink(std::string_view(episode.chunks).substr(start, 7))) {
                    break; // 客户端主动停（maxEvents 已达）
                }
            }
        } else {
            response.body = episode.chunks;
        }
        return response;
    }
};

// 两条完整 SSE 事件的字节流（server 形态：id/event/data + 空行）。
std::string sseEvent(const std::string& id, const std::string& type, const std::string& dataJson) {
    return "id: " + id + "\nevent: " + type + "\ndata: " + dataJson + "\n\n";
}

// AttemptFn 需要可调用对象；connect 是成员函数，包一层。
auto bindConnect(ScriptedStream& stream) {
    return [&stream](const HttpRequest& request, const ChunkSink& sink) {
        return stream.connect(request, sink);
    };
}

ListenOptions makeOptions(int maxEvents) {
    ListenOptions options;
    options.url = "https://api.test/api/v1/workspaces/ws_1/events";
    options.maxEvents = maxEvents;
    options.initialBackoff = std::chrono::seconds(1);
    options.maxBackoff = std::chrono::seconds(30);
    return options;
}

std::vector<long long> sleepMillis(const std::vector<std::chrono::milliseconds>& sleeps) {
    std::vector<long long> out;
    for (const auto& sleep : sleeps) {
        out.push_back(sleep.count());
    }
    return out;
}

TEST_CASE("event listen streams JSON lines and stops at max-events", "[event]") {
    ScriptedStream stream;
    stream.episodes.push_back(
        {200,
         sseEvent(
             "evt_a", "task.created",
             R"({"id":"evt_a","type":"task.created","occurred_at":"2026-09-12T00:00:01Z","workspace_id":"ws_1","data":{}})") +
             sseEvent(
                 "evt_b", "task.updated",
                 R"({"id":"evt_b","type":"task.updated","occurred_at":"2026-09-12T00:00:02Z","workspace_id":"ws_1","data":{}})"),
         false});

    std::ostringstream out;
    std::ostringstream err;
    std::vector<std::chrono::milliseconds> sleeps;
    const int code = runListenLoop(
        makeOptions(2), bindConnect(stream),
        [&](std::chrono::milliseconds d) { sleeps.push_back(d); }, out, err, /*json=*/true);
    REQUIRE(code == 0);
    REQUIRE(sleeps.empty()); // 一次连接拿到全部事件，无重连

    // --json：每行一个原始 envelope。
    const auto line1 = nlohmann::json::parse(out.str().substr(0, out.str().find('\n')));
    REQUIRE(line1.at("id") == "evt_a");
    std::size_t lines = 0;
    for (std::size_t i = 0; i < out.str().size(); ++i) {
        if (out.str()[i] == '\n') {
            ++lines;
        }
    }
    REQUIRE(lines == 2);
    // 首连无 Last-Event-ID。
    REQUIRE(stream.lastEventIds.size() == 1);
    REQUIRE(stream.lastEventIds[0].empty());
}

TEST_CASE("event listen resumes with Last-Event-ID after a server close", "[event]") {
    ScriptedStream stream;
    stream.episodes.push_back(
        {200, sseEvent("evt_a", "task.created", R"({"id":"evt_a","type":"task.created"})"), false});
    stream.episodes.push_back(
        {200, sseEvent("evt_b", "task.updated", R"({"id":"evt_b","type":"task.updated"})"), false});

    std::ostringstream out;
    std::ostringstream err;
    std::vector<std::chrono::milliseconds> sleeps;
    const int code = runListenLoop(
        makeOptions(2), bindConnect(stream),
        [&](std::chrono::milliseconds d) { sleeps.push_back(d); }, out, err, /*json=*/true);
    REQUIRE(code == 0);
    REQUIRE(stream.lastEventIds.size() == 2);
    REQUIRE(stream.lastEventIds[0].empty());
    REQUIRE(stream.lastEventIds[1] == "evt_a"); // 断线续传游标
    // 第一次 200 关流触发一次退避重连。
    REQUIRE(sleepMillis(sleeps) == std::vector<long long>{1000});
}

TEST_CASE("event listen keeps SSE frames split across chunks intact", "[event]") {
    // 单事件字节流被切成任意小块后仍恰好产出一次（FrameParser 集成）。
    const std::string payload = sseEvent("evt_x", "message.created", R"({"id":"evt_x"})");
    ScriptedStream stream;
    stream.episodes.push_back({200, payload, false});

    std::ostringstream out;
    std::ostringstream err;
    const int code = runListenLoop(
        makeOptions(1), bindConnect(stream), [&](std::chrono::milliseconds) {}, out, err,
        /*json=*/true);
    REQUIRE(code == 0);
    REQUIRE(nlohmann::json::parse(out.str()).at("id") == "evt_x");
}

TEST_CASE("event listen snapshot.required resets the resume cursor", "[event]") {
    ScriptedStream stream;
    // 第一连：正常事件推进游标到 evt_a，随后控制事件 snapshot.required。
    stream.episodes.push_back(
        {200,
         sseEvent("evt_a", "task.created", R"({"id":"evt_a","type":"task.created}")") +
             sseEvent(
                 "evt_snapshot_required", "snapshot.required",
                 R"({"id":"evt_snapshot_required","type":"snapshot.required","data":{"reason":"cursor_expired"}})"),
         false});
    stream.episodes.push_back(
        {200, sseEvent("evt_b", "task.updated", R"({"id":"evt_b","type":"task.updated"})"), false});

    std::ostringstream out;
    std::ostringstream err;
    const int code = runListenLoop(
        makeOptions(2), bindConnect(stream), [&](std::chrono::milliseconds) {}, out, err,
        /*json=*/true);
    REQUIRE(code == 0);
    REQUIRE(stream.lastEventIds.size() == 2);
    // snapshot.required 之后游标必须清空，绝不携带过期 evt_snapshot_required。
    REQUIRE(stream.lastEventIds[1].empty());
}

TEST_CASE("event listen retries transport errors with growing backoff", "[event]") {
    ScriptedStream stream;
    stream.episodes.push_back({200, "", true}); // transport error
    stream.episodes.push_back({200, sseEvent("evt_a", "task.created", R"({"id":"evt_a"})"), false});

    std::ostringstream out;
    std::ostringstream err;
    std::vector<std::chrono::milliseconds> sleeps;
    const int code = runListenLoop(
        makeOptions(1), bindConnect(stream),
        [&](std::chrono::milliseconds d) { sleeps.push_back(d); }, out, err, /*json=*/true);
    REQUIRE(code == 0);
    REQUIRE(sleepMillis(sleeps) == std::vector<long long>{1000});
    REQUIRE(err.str().find("stream interrupted") != std::string::npos);
}

TEST_CASE("event listen retries 5xx but fails fast on 404", "[event]") {
    ScriptedStream stream5xx;
    stream5xx.episodes.push_back(
        {503, astral_test::errorEnvelopeBody("UNAVAILABLE", "overloaded"), false});
    stream5xx.episodes.push_back(
        {200, sseEvent("evt_a", "task.created", R"({"id":"evt_a"})"), false});
    std::ostringstream out;
    std::ostringstream err;
    std::vector<std::chrono::milliseconds> sleeps;
    const int code = runListenLoop(
        makeOptions(1), bindConnect(stream5xx),
        [&](std::chrono::milliseconds d) { sleeps.push_back(d); }, out, err, /*json=*/true);
    REQUIRE(code == 0);
    REQUIRE(sleepMillis(sleeps) == std::vector<long long>{1000});

    // 404（非成员：WORKSPACE_NOT_FOUND，本轮回新增的授权语义）→ 直接终止，
    // 带协议 envelope 透传。
    ScriptedStream stream404;
    stream404.episodes.push_back(
        {404, astral_test::errorEnvelopeBody("WORKSPACE_NOT_FOUND", "workspace not found"), false});
    std::ostringstream out404;
    std::ostringstream err404;
    bool caught = false;
    try {
        runListenLoop(
            makeOptions(1), bindConnect(stream404), [&](std::chrono::milliseconds) {}, out404,
            err404, /*json=*/true);
    } catch (const astral::core::AstralError& error) {
        caught = true;
        REQUIRE(error.exitCode() == static_cast<int>(astral::core::ExitCode::NotFound));
        REQUIRE(error.protocolCode() == std::optional<std::string>("WORKSPACE_NOT_FOUND"));
    }
    REQUIRE(caught);
}

TEST_CASE("event listen human mode prints time type id", "[event]") {
    ScriptedStream stream;
    stream.episodes.push_back(
        {200,
         sseEvent("evt_a", "task.created",
                  R"({"id":"evt_a","type":"task.created","occurred_at":"2026-09-12T00:00:01Z"})"),
         false});
    std::ostringstream out;
    std::ostringstream err;
    const int code = runListenLoop(
        makeOptions(1), bindConnect(stream), [&](std::chrono::milliseconds) {}, out, err,
        /*json=*/false);
    REQUIRE(code == 0);
    REQUIRE(out.str().find("2026-09-12T00:00:01Z  task.created  evt_a") != std::string::npos);
}

} // namespace
