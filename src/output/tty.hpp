#pragma once

#include <string>

namespace astral::output {

bool stdoutIsTty();
bool stderrIsTty();

// Enable ANSI escape processing on consoles that need it (Windows legacy
// consoles); safe no-op elsewhere.
void enableNativeAnsi();

// Resolve the --color flag: "auto" defers to TTY detection and NO_COLOR;
// JSON output is never colored. See ARCHITECTURE.md section 12.
bool shouldUseColor(const std::string& colorMode, bool jsonMode);

} // namespace astral::output
