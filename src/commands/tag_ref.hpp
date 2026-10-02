#pragma once

// <tag> 参数解析的共享实现：`tag_` 前缀直接当 id；否则对
// GET /workspaces/{id}/tags 的词典做规范化名匹配（服务端 NFKC+lowercase，
// 覆盖纯 ASCII 的 CLI 输入），找不到本地报 NotFound。todo attach/detach
// 与 tags rename/delete 原各自持有一份逐字节相同的文件内私有实现，现提升
// 至此——名匹配规则只允许有一处事实来源。

#include <cctype>
#include <string>

#include <nlohmann/json.hpp>

#include "auth/api.hpp"
#include "client/http_client.hpp"
#include "core/error.hpp"
#include "output/render.hpp"

namespace astral::commands {

struct TagRef {
    std::string id;
    std::string name;
};

inline TagRef resolveTag(const auth::ApiSession& api, const auth::WorkspaceContext& ws,
                         const std::string& nameOrId) {
    if (nameOrId.rfind("tag_", 0) == 0) {
        return TagRef{nameOrId, ""};
    }
    client::HttpRequest request;
    request.url = auth::apiUrl(api, "/workspaces/" + ws.workspaceId + "/tags");
    const nlohmann::json page =
        nlohmann::json::parse(api.requireSuccess(std::move(request), "tag list").body);
    std::string lower;
    lower.reserve(nameOrId.size());
    for (char c : nameOrId) {
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (const auto it = page.find("items"); it != page.end() && it->is_array()) {
        for (const auto& tag : *it) {
            const std::string name = output::scalarOr(tag, "name");
            std::string candidate;
            candidate.reserve(name.size());
            for (char c : name) {
                candidate += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (candidate == lower) {
                return TagRef{output::scalarOr(tag, "id"), name};
            }
        }
    }
    throw core::AstralError(core::Errc::NotFound,
                            "tag '" + nameOrId + "' not found in this workspace");
}

} // namespace astral::commands
