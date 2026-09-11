#pragma once

#include <chrono>
#include <functional>
#include <ostream>
#include <string>

#include "client/http_client.hpp"
#include "client/sse.hpp"

namespace astral::events {

// Opens one streaming connection attempt and feeds body chunks to onChunk as
// they arrive (libcurl sendStreaming in production, scripted fakes in tests).
// Transport failures throw AstralError(NETWORK_ERROR/TIMEOUT); any HTTP
// status is returned for the loop to classify.
using AttemptFn = std::function<client::HttpResponse(const client::HttpRequest& request,
                                                     const client::ChunkSink& onChunk)>;

// Seam for polling waits; unit tests record durations instead of sleeping.
using SleepFn = std::function<void(std::chrono::milliseconds)>;

struct ListenOptions {
    std::string url;   // absolute events URL (origin + apiBase + path)
    int maxEvents = 0; // 0 = run until interrupted (Ctrl+C kills the process)
    std::chrono::milliseconds initialBackoff{std::chrono::seconds(1)};
    std::chrono::milliseconds maxBackoff{std::chrono::seconds(30)};
};

// Reconnecting SSE consumer (ARCHITECTURE.md section 13, protocol.md
// section 5): prints one line per event to `out` — raw envelope JSON lines
// under --json, human-readable `time type id` otherwise — and resumes with
// Last-Event-ID across reconnects. The cursor resets on snapshot.required so
// the next attempt replays nothing stale. Transient failures (transport
// errors, 429, 5xx) retry with exponential backoff; terminal rejections
// (401/403/404 and other 4xx) surface as AstralError with the server's
// protocol envelope passthrough. Returns exit code 0 on a clean stop.
int runListenLoop(const ListenOptions& options, const AttemptFn& attempt, const SleepFn& sleep,
                  std::ostream& out, std::ostream& err, bool json);

} // namespace astral::events
