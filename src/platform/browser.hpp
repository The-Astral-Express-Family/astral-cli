#pragma once

#include <string>

namespace astral::platform {

// Opens a URL in the user's preferred browser (xdg-open / open / start).
// Hard kill switch: ASTRAL_NO_BROWSER=<non-empty> makes this a no-op returning
// false - tests and CI must set it (see tests/unit/support/no_browser.cpp).
// Only call this in interactive human flows; JSON/non-TTY modes must never
// spawn a browser. Returns false when no launcher is available.
bool openInBrowser(const std::string& url);

} // namespace astral::platform
