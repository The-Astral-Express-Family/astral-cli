#pragma once

#include <string>

namespace astral::platform {

// Opens a URL in the user's preferred browser (xdg-open / open / start).
// Only call this in interactive human flows; JSON/non-TTY modes must never
// spawn a browser. Returns false when no launcher is available.
bool openInBrowser(const std::string& url);

} // namespace astral::platform
