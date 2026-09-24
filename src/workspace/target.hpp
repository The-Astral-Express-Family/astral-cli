#pragma once

#include <optional>
#include <string>

namespace astral::workspace {

// Parses the `astral init <server-url>/<workspace-name>` shorthand
// (ARCHITECTURE.md section 9.1): the last path segment becomes the workspace
// candidate unless --workspace is given. No networking, no fallback discovery
// against the full URL — init requires a workspace name and refuses to guess.
struct TargetSpec {
    // The input interpreted as a pure server URL (normalized).
    std::string fullUrl;

    // The server URL with the workspace segment removed (nullopt when the
    // input had no split point). init consumes it for discovery (falling
    // back to fullUrl when no split happened, i.e. explicit --workspace).
    std::optional<std::string> urlAfterSplit;

    // The split-off segment, present only when urlAfterSplit is set or when
    // an explicit --workspace was given.
    std::optional<std::string> workspaceName;
};

// Normalizes a server URL: requires a scheme, lowercases scheme and host,
// drops a trailing slash. Returns nullopt for inputs without a scheme.
std::optional<std::string> normalizeServerUrl(const std::string& raw);

// `explicitWorkspace` comes from `--workspace` and wins outright: the whole
// input is then treated as the server URL (rule 1 of section 9.1).
TargetSpec parseTargetSpec(const std::string& input,
                           const std::optional<std::string>& explicitWorkspace);

} // namespace astral::workspace
