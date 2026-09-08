#pragma once

#include <iosfwd>

namespace astral::app {

// Runs the CLI with injected output streams; returns the process exit code.
// Used directly by unit tests to pin the JSON contract and exit codes.
int runApp(int argc, char** argv, std::ostream& out, std::ostream& err);

// Console entry point (real stdout/stderr + platform setup).
int runMain(int argc, char** argv);

} // namespace astral::app
