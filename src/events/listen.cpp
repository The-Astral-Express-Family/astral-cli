#include "events/listen.hpp"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "auth/api.hpp"
#include "core/error.hpp"

namespace astral::events {

namespace {

using client::sse::Event;

std::chrono::milliseconds growBackoff(std::chrono::milliseconds current,
                                      std::chrono::milliseconds max) {
    return std::min(current * 2, max);
}

// One event -> one output line. --json passes the envelope through verbatim
// (machine contract); human mode summarizes.
void emitLine(std::ostream& out, std::string_view data, bool jsonMode) {
    if (jsonMode) {
        out << data << '\n';
        out.flush(); // downstream (pipe/jq) expects live output
        return;
    }
    std::string type = "message";
    std::string id;
    std::string at;
    try {
        const nlohmann::json envelope = nlohmann::json::parse(data);
        type = envelope.value("type", type);
        id = envelope.value("id", std::string());
        at = envelope.value("occurred_at", std::string());
    } catch (const std::exception&) {
        // Non-JSON payload: show it raw rather than dropping the event.
        out << data << '\n';
        out.flush();
        return;
    }
    out << at << "  " << type << "  " << id << '\n';
    out.flush();
}

} // namespace

int runListenLoop(const ListenOptions& options, const AttemptFn& attempt, const SleepFn& sleep,
                  std::ostream& out, std::ostream& err, bool json) {
    // Resume cursor across reconnects; empty = connect without Last-Event-ID.
    std::optional<std::string> cursor;
    std::chrono::milliseconds backoff = options.initialBackoff;
    int deliveredTotal = 0; // domain events, across all connections

    while (true) {
        client::sse::FrameParser parser;
        bool deliveredThisConnection = false;
        bool wantStop = false;

        auto processEvent = [&](Event event) {
            if (event.name == "snapshot.required") {
                // Control event, not a domain fact: surface it but don't
                // count it toward max-events. The cursor predates the
                // retention window, so drop it - the next attempt replays
                // nothing stale and the consumer re-snapshots.
                cursor.reset();
                emitLine(out, event.data, json);
                return;
            }
            if (!event.id.empty()) {
                cursor = event.id;
            }
            ++deliveredTotal;
            deliveredThisConnection = true;
            emitLine(out, event.data, json);
            if (options.maxEvents != 0 && deliveredTotal >= options.maxEvents) {
                wantStop = true;
            }
        };

        client::HttpRequest request;
        request.method = "GET";
        request.url = options.url;
        if (cursor) {
            request.headers.emplace_back("Last-Event-ID", *cursor);
        }

        client::HttpResponse response;
        try {
            response = attempt(request, [&](std::string_view chunk) {
                parser.feed(chunk);
                for (auto& event : parser.takeEvents()) {
                    processEvent(std::move(event));
                    if (wantStop) {
                        return false;
                    }
                }
                return true;
            });
        } catch (const core::AstralError& error) {
            // Transport-level failure (NETWORK_ERROR/TIMEOUT): transient by
            // definition, back off and reconnect; everything else is fatal.
            if (error.code() != core::Errc::NetworkError && error.code() != core::Errc::Timeout) {
                throw;
            }
            err << "astral: stream interrupted (" << error.what() << "); reconnecting in "
                << std::chrono::duration_cast<std::chrono::seconds>(backoff).count() << "s\n";
            sleep(backoff);
            backoff = growBackoff(backoff, options.maxBackoff);
            continue;
        }

        if (wantStop) {
            return 0; // maxEvents reached: clean stop, not a disconnect
        }
        if (response.status == 200) {
            // Server closed a healthy stream (deploy/restart/timeout): resume
            // from the cursor. A connection that delivered events proved the
            // path works, so reset the backoff for the next hiccup.
            if (deliveredThisConnection) {
                backoff = options.initialBackoff;
            }
            err << "astral: stream closed by server; reconnecting in "
                << std::chrono::duration_cast<std::chrono::seconds>(backoff).count() << "s\n";
            sleep(backoff);
            backoff = growBackoff(backoff, options.maxBackoff);
            continue;
        }
        if (response.status == 429 || response.status >= 500) {
            // Transient server-side condition: honor Retry-After when present.
            std::chrono::milliseconds wait = backoff;
            if (const auto retryAfter = response.header("Retry-After")) {
                const long seconds = std::strtol(retryAfter->c_str(), nullptr, 10);
                if (seconds > 0) {
                    wait = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::seconds(seconds));
                }
            }
            err << "astral: event stream unavailable (HTTP " << response.status << "); retrying in "
                << std::chrono::duration_cast<std::chrono::seconds>(wait).count() << "s\n";
            sleep(wait);
            backoff = growBackoff(backoff, options.maxBackoff);
            continue;
        }
        // 401 (after the caller's lazy refresh), 403, 404 and every other 4xx
        // will not heal by retrying: map through the standard protocol error
        // path (exit code + envelope passthrough) and stop.
        auth::throwApiError(response, "event stream");
    }
}

} // namespace astral::events
