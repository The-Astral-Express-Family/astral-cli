#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace astral::workspace {

namespace fs = std::filesystem;

// The repo-local, non-secret binding file `.astral/config.json`
// (ARCHITECTURE.md section 8). IDs are authoritative; URL/name/slug exist
// for discovery and human display.
struct Binding {
    int version = 1;
    std::string serverId;
    std::string serverUrl;
    std::string workspaceId;
    std::string workspaceName;
    std::string workspaceSlug;
};

// Parses the binding file at `dir/.astral/config.json`.
// Returns nullopt when the file does not exist; throws AstralError
// (LOCAL_WORKSPACE_ERROR) when it exists but is malformed.
std::optional<Binding> readBinding(const fs::path& dir);

// Walks upward from `from` (inclusive) and returns the first directory that
// carries a binding file.
std::optional<fs::path> findBindingDir(const fs::path& from);

// Writes the binding atomically (temp file + rename), creating `.astral/`
// when needed. Throws AstralError (LOCAL_WORKSPACE_ERROR) on failure; on
// failure no partial binding is left behind.
void writeBinding(const fs::path& dir, const Binding& binding);

// Same server id + workspace id means the same binding regardless of
// display fields.
bool sameTarget(const Binding& left, const Binding& right);

} // namespace astral::workspace
