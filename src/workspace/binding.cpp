#include "workspace/binding.hpp"

#include <fstream>

#include <nlohmann/json.hpp>

#include "core/error.hpp"
#include "platform/atomic_file.hpp"

namespace astral::workspace {

namespace {

constexpr int kBindingVersion = 1;

Binding parseBinding(const nlohmann::json& root) {
    Binding binding;
    binding.version = root.value("version", 0);
    if (binding.version != kBindingVersion) {
        throw core::AstralError(core::Errc::LocalWorkspaceError,
                                "unsupported .astral/config.json version " +
                                    std::to_string(binding.version));
    }
    const auto server = root.find("server");
    const auto workspace = root.find("workspace");
    if (server == root.end() || workspace == root.end() || !server->is_object() ||
        !workspace->is_object()) {
        throw core::AstralError(core::Errc::LocalWorkspaceError,
                                ".astral/config.json is missing server/workspace objects");
    }
    binding.serverId = server->at("id").get<std::string>();
    binding.serverUrl = server->at("url").get<std::string>();
    binding.workspaceId = workspace->at("id").get<std::string>();
    binding.workspaceName = workspace->value("name", std::string());
    binding.workspaceSlug = workspace->value("slug", std::string());
    if (binding.serverId.empty() || binding.serverUrl.empty() || binding.workspaceId.empty()) {
        throw core::AstralError(core::Errc::LocalWorkspaceError,
                                ".astral/config.json has empty server/workspace identifiers");
    }
    return binding;
}

} // namespace

std::optional<Binding> readBinding(const fs::path& dir) {
    const fs::path file = dir / ".astral" / "config.json";
    std::error_code ec;
    if (!fs::exists(file, ec)) {
        return std::nullopt;
    }
    std::ifstream input(file);
    if (!input) {
        throw core::AstralError(core::Errc::LocalWorkspaceError,
                                "cannot open " + file.string() + " for reading");
    }
    try {
        nlohmann::json root = nlohmann::json::parse(input);
        return parseBinding(root);
    } catch (const nlohmann::json::exception& e) {
        throw core::AstralError(core::Errc::LocalWorkspaceError,
                                "malformed .astral/config.json: " + std::string(e.what()));
    }
}

std::optional<fs::path> findBindingDir(const fs::path& from) {
    std::error_code ec;
    fs::path dir = fs::absolute(from, ec);
    if (ec) {
        return std::nullopt;
    }
    while (true) {
        if (fs::exists(dir / ".astral" / "config.json", ec)) {
            return dir;
        }
        const fs::path parent = dir.parent_path();
        if (parent == dir) {
            return std::nullopt;
        }
        dir = parent;
    }
}

void writeBinding(const fs::path& dir, const Binding& binding) {
    const fs::path astralDir = dir / ".astral";
    std::error_code ec;
    fs::create_directories(astralDir, ec);
    if (ec) {
        throw core::AstralError(core::Errc::LocalWorkspaceError,
                                "cannot create " + astralDir.string() + ": " + ec.message());
    }

    const nlohmann::json root = {
        {"version", kBindingVersion},
        {"server",
         {
             {"id", binding.serverId},
             {"url", binding.serverUrl},
         }},
        {"workspace",
         {
             {"id", binding.workspaceId},
             {"name", binding.workspaceName},
             {"slug", binding.workspaceSlug},
         }},
    };

    platform::writeFileAtomic(astralDir / "config.json", root.dump(2) + '\n',
                              core::Errc::LocalWorkspaceError, /*ownerOnly=*/false);
}

bool sameTarget(const Binding& left, const Binding& right) {
    return left.serverId == right.serverId && left.workspaceId == right.workspaceId;
}

} // namespace astral::workspace
