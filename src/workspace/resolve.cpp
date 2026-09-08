#include "workspace/resolve.hpp"

namespace astral::workspace {

ResolveResult resolveTarget(const ResolveInput& input) {
    if (input.flagServer && input.flagWorkspace) {
        ResolvedTarget target;
        target.serverUrl = *input.flagServer;
        target.workspaceName = *input.flagWorkspace;
        target.source = "flag";
        return ResolveResult{true, target, ""};
    }
    if (input.flagServer && !input.flagWorkspace) {
        return ResolveResult::failed("--server given without --workspace; a workspace is required");
    }
    if (!input.flagServer && input.flagWorkspace) {
        return ResolveResult::failed("--workspace given without --server; cannot guess the server");
    }

    if (input.localBinding) {
        const Binding& binding = *input.localBinding;
        ResolvedTarget target;
        target.serverUrl = binding.serverUrl;
        target.serverId = binding.serverId;
        target.workspaceId = binding.workspaceId;
        target.workspaceName = binding.workspaceName;
        target.source = "binding";
        return ResolveResult{true, target, ""};
    }

    if (input.envServer && input.envWorkspace) {
        ResolvedTarget target;
        target.serverUrl = *input.envServer;
        target.workspaceName = *input.envWorkspace;
        target.source = "env";
        return ResolveResult{true, target, ""};
    }

    return ResolveResult::failed(
        "no server/workspace target: pass --server/--workspace, run `astral init`, "
        "or set ASTRAL_SERVER/ASTRAL_WORKSPACE");
}

} // namespace astral::workspace
