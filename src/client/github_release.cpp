#include "client/github_release.hpp"

#include <nlohmann/json.hpp>

#include "core/error.hpp"

namespace astral::client {

namespace {

using nlohmann::json;

constexpr const char* kGitHubApiBase = "https://api.github.com/repos";

std::string requiredString(const json& root, const char* key) {
    const auto it = root.find(key);
    if (it == root.end() || !it->is_string()) {
        throw core::AstralError(core::Errc::NetworkError,
                                std::string("malformed release JSON: missing '") + key + "'");
    }
    return it->get<std::string>();
}

} // namespace

ReleaseInfo parseReleaseJson(const std::string& jsonText) {
    json root;
    try {
        root = json::parse(jsonText);
    } catch (const json::exception& e) {
        throw core::AstralError(core::Errc::NetworkError,
                                std::string("malformed release JSON: ") + e.what());
    }
    if (!root.is_object()) {
        throw core::AstralError(core::Errc::NetworkError,
                                "malformed release JSON: expected an object");
    }

    ReleaseInfo info;
    info.tagName = requiredString(root, "tag_name");

    const auto assets = root.find("assets");
    if (assets != root.end() && assets->is_array()) {
        for (const auto& asset : *assets) {
            if (!asset.is_object()) {
                continue;
            }
            const auto downloadUrl = asset.find("browser_download_url");
            if (downloadUrl == asset.end() || !downloadUrl->is_string()) {
                continue;
            }
            // name is optional in the API schema; fall back to a generic label.
            std::string name = "asset";
            if (const auto nameIt = asset.find("name");
                nameIt != asset.end() && nameIt->is_string()) {
                name = nameIt->get<std::string>();
            }
            info.assets.push_back(ReleaseAsset{std::move(name), downloadUrl->get<std::string>()});
        }
    }
    return info;
}

GithubReleaseClient::GithubReleaseClient(HttpClient& http) : http_(http) {}

ReleaseInfo GithubReleaseClient::latest(std::string_view repo) {
    return fetchRelease(std::string(kGitHubApiBase) + "/" + std::string(repo) + "/releases/latest",
                        "latest");
}

ReleaseInfo GithubReleaseClient::byTag(std::string_view repo, std::string_view tag) {
    return fetchRelease(std::string(kGitHubApiBase) + "/" + std::string(repo) + "/releases/tags/" +
                            std::string(tag),
                        std::string(tag));
}

ReleaseInfo GithubReleaseClient::fetchRelease(const std::string& url, const std::string& tagLabel) {
    const HttpResponse response = http_.get(url);
    if (response.status == 404) {
        throw core::AstralError(core::Errc::ServerNotFound, "version " + tagLabel + " not found");
    }
    if (response.status == 403) {
        throw core::AstralError(core::Errc::NetworkError,
                                "GitHub API rate limit exceeded (HTTP 403): " + url);
    }
    if (response.status < 200 || response.status >= 300) {
        throw core::AstralError(core::Errc::NetworkError,
                                "GitHub request failed (HTTP " + std::to_string(response.status) +
                                    "): " + url);
    }
    return parseReleaseJson(response.body);
}

} // namespace astral::client
