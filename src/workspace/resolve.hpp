#pragma once

#include <optional>
#include <string>

#include "workspace/binding.hpp"

namespace astral::workspace {

// Inputs to target resolution (ARCHITECTURE.md section 10); all fields
// are explicit so the resolver is pure and trivially testable.
struct ResolveInput {
    std::optional<std::string> flagServer;
    std::optional<std::string> flagWorkspace;
    std::optional<Binding> localBinding;
    std::optional<std::string> envServer;
    std::optional<std::string> envWorkspace;
};

struct ResolvedTarget {
    std::string serverUrl;
    std::string serverId;
    std::string workspaceId;
    std::string workspaceName;
    // "flag", "binding", or "env" - where the winning values came from.
    std::string source;
};

struct ResolveResult {
    bool ok = false;
    ResolvedTarget target;
    std::string failureReason;

    static ResolveResult failed(std::string reason) {
        ResolveResult result;
        result.failureReason = std::move(reason);
        return result;
    }
};

// Precedence (section 10): explicit flags > repo binding > env vars.
// A repo binding is never silently overridden by "most recently logged in"
// servers; only explicit flags may override it.
ResolveResult resolveTarget(const ResolveInput& input);

} // namespace astral::workspace
