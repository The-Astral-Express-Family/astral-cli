#pragma once

#include <optional>
#include <string>

namespace astral::workspace {

// Parses the `astral init <server_url>[/<workspace_name>]` shorthand
// (ARCHITECTURE.md section 9.1). Discovery is attempted against the full URL
// first; only if that fails is the last path segment split off as a
// workspace-name candidate. This type carries both candidates without doing
// any networking.
struct TargetSpec {
    // The input interpreted as a pure server URL (also what discovery tries
    // first).
    std::string fullUrl;

    // The input with its last path segment removed; nullopt when there is
    // nothing to split (no path segments beyond "/").
    std::optional<std::string> urlAfterSplit;

    // The split-off segment, valid only when urlAfterSplit is set or when an
    // explicit --workspace was given.
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
