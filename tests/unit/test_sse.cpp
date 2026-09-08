#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "client/sse.hpp"

using astral::client::sse::FrameParser;

TEST_CASE("a complete frame dispatches one event") {
    FrameParser parser;
    parser.feed("id: 42\nevent: ping\ndata: hello\n\n");

    const auto events = parser.takeEvents();
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].id == "42");
    REQUIRE(events[0].name == "ping");
    REQUIRE(events[0].data == "hello");
    REQUIRE(parser.lastEventId() == "42");
}

TEST_CASE("data lines join with newlines") {
    FrameParser parser;
    parser.feed("data: line one\ndata: line two\n\n");

    const auto events = parser.takeEvents();
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].data == "line one\nline two");
    REQUIRE(events[0].name == "message");
}

TEST_CASE("chunks split mid-line are reassembled") {
    FrameParser parser;
    parser.feed("id: 7\ndata: he");
    REQUIRE(parser.takeEvents().empty());

    parser.feed("llo\n\n");
    const auto events = parser.takeEvents();
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].data == "hello");
    REQUIRE(parser.lastEventId() == "7");
}

TEST_CASE("comments and keep-alives produce no events") {
    FrameParser parser;
    parser.feed(": keep-alive\n\n");
    REQUIRE(parser.takeEvents().empty());
}

TEST_CASE("CRLF line endings are handled") {
    FrameParser parser;
    parser.feed("event: tick\r\ndata: 1\r\n\r\n");
    const auto events = parser.takeEvents();
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].name == "tick");
    REQUIRE(events[0].data == "1");
}

TEST_CASE("an id-only block updates lastEventId without dispatching") {
    FrameParser parser;
    parser.feed("id: 99\n\n");
    REQUIRE(parser.takeEvents().empty());
    REQUIRE(parser.lastEventId() == "99");
}

TEST_CASE("events without ids keep the previous lastEventId") {
    FrameParser parser;
    parser.feed("id: 1\ndata: a\n\ndata: b\n\n");
    const auto events = parser.takeEvents();
    REQUIRE(events.size() == 2);
    REQUIRE(events[1].id.empty());
    REQUIRE(parser.lastEventId() == "1");
}
