#pragma once

#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace astral::client::sse {

// One dispatched server-sent event; field semantics follow the SSE spec.
struct Event {
    std::string id;
    std::string name = "message";
    std::string data;
};

// Incremental SSE frame parser: feed arbitrary network chunks, receive
// complete events. Pure logic - no sockets - so reconnect + last-event-id
// behavior can be built on top and unit-tested independently.
class FrameParser {
public:
    // Feed a raw chunk; may contain zero or more partial/complete lines.
    void feed(std::string_view chunk);

    // Hands out all events completed by the chunks fed so far.
    std::vector<Event> takeEvents();

    // The id of the last dispatched event, for Last-Event-ID on reconnect.
    const std::string& lastEventId() const { return lastEventId_; }

    void reset();

private:
    void processLine(std::string_view line);
    void dispatch();

    std::string buffer_;
    std::string pendingId_;
    std::string pendingName_;
    std::vector<std::string> pendingDataLines_;
    std::string lastEventId_;
    std::deque<Event> ready_;
};

} // namespace astral::client::sse
