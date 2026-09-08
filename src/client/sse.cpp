#include "client/sse.hpp"

namespace astral::client::sse {

void FrameParser::feed(std::string_view chunk) {
    buffer_.append(chunk.data(), chunk.size());
    size_t consumed = 0;
    while (true) {
        const auto end = buffer_.find('\n', consumed);
        if (end == std::string::npos) {
            break;
        }
        std::string_view line(buffer_.data() + consumed, end - consumed);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        processLine(line);
        consumed = end + 1;
    }
    buffer_.erase(0, consumed);
}

void FrameParser::processLine(std::string_view line) {
    if (line.empty()) {
        dispatch();
        return;
    }
    if (line.front() == ':') {
        return; // comment / keep-alive
    }

    std::string_view field = line;
    std::string_view value;
    const auto colon = line.find(':');
    if (colon != std::string_view::npos) {
        field = line.substr(0, colon);
        value = line.substr(colon + 1);
        if (!value.empty() && value.front() == ' ') {
            value.remove_prefix(1);
        }
    }

    if (field == "event") {
        pendingName_ = std::string(value);
    } else if (field == "data") {
        pendingDataLines_.emplace_back(value);
    } else if (field == "id") {
        if (value.find('\n') == std::string_view::npos) {
            pendingId_ = std::string(value);
        }
    }
    // "retry" and unknown fields are ignored for now.
}

void FrameParser::dispatch() {
    if (pendingDataLines_.empty()) {
        // An event block without data lines sets the id but dispatches nothing.
        if (!pendingId_.empty()) {
            lastEventId_ = pendingId_;
        }
        pendingId_.clear();
        pendingName_.clear();
        return;
    }
    Event event;
    event.id = pendingId_;
    event.name = pendingName_.empty() ? "message" : pendingName_;
    for (size_t i = 0; i < pendingDataLines_.size(); ++i) {
        if (i > 0) {
            event.data += '\n';
        }
        event.data += pendingDataLines_[i];
    }
    if (!event.id.empty()) {
        lastEventId_ = event.id;
    }
    ready_.push_back(std::move(event));
    pendingId_.clear();
    pendingName_.clear();
    pendingDataLines_.clear();
}

std::vector<Event> FrameParser::takeEvents() {
    std::vector<Event> events(std::make_move_iterator(ready_.begin()),
                              std::make_move_iterator(ready_.end()));
    ready_.clear();
    return events;
}

void FrameParser::reset() {
    buffer_.clear();
    pendingId_.clear();
    pendingName_.clear();
    pendingDataLines_.clear();
    ready_.clear();
}

} // namespace astral::client::sse
