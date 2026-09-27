#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "client/http_client.hpp"

namespace astral::client {

struct ReleaseAsset {
    std::string name;
    std::string downloadUrl;
};

struct ReleaseInfo {
    std::string tagName;
    std::vector<ReleaseAsset> assets;
};

// Pure parsing (offline-testable): GitHub releases API JSON -> ReleaseInfo.
// Reads tag_name and assets[].{name, browser_download_url}. Throws
// AstralError(NETWORK_ERROR) on malformed input or missing tag_name; asset
// entries missing browser_download_url are skipped.
ReleaseInfo parseReleaseJson(const std::string& json);

// Fetches a GitHub release description for <repo> ("owner/name"). The
// underlying HttpClient already sends the astral-cli user agent (Options) and
// GitHub returns JSON for these endpoints without a custom Accept header.
// Status mapping: 404 -> SERVER_NOT_FOUND ("version <tag> not found"),
// 403 -> NETWORK_ERROR with a rate-limit hint, other HTTP failures and
// transport errors -> NETWORK_ERROR (transport errors surface as-is).
class GithubReleaseClient {
public:
    explicit GithubReleaseClient(HttpClient& http);

    ReleaseInfo latest(std::string_view repo);
    ReleaseInfo byTag(std::string_view repo, std::string_view tag);

private:
    ReleaseInfo fetchRelease(const std::string& url, const std::string& tagLabel);

    HttpClient& http_;
};

} // namespace astral::client
