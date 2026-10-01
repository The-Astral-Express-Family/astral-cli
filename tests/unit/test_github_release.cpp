#include <catch2/catch_test_macros.hpp>

#include <string>

#include "client/github_release.hpp"
#include "core/error.hpp"

using astral::client::parseReleaseJson;
using astral::client::ReleaseInfo;

namespace {

// Truncated but field-complete shape of a real
// GET /repos/<owner>/<repo>/releases/latest response.
const std::string kLatestReleaseJson = R"json({
  "url": "https://api.github.com/repos/The-Astral-Express-Family/astral-cli/releases/197107626",
  "html_url": "https://github.com/The-Astral-Express-Family/astral-cli/releases/tag/v0.2.0",
  "assets_url": "https://api.github.com/repos/The-Astral-Express-Family/astral-cli/releases/197107626/assets",
  "tag_name": "v0.2.0",
  "target_commitish": "main",
  "name": "astral-cli v0.2.0",
  "draft": false,
  "prerelease": false,
  "created_at": "2026-09-01T10:00:00Z",
  "published_at": "2026-09-01T10:05:00Z",
  "assets": [
    {
      "url": "https://api.github.com/repos/The-Astral-Express-Family/astral-cli/releases/assets/83452101",
      "id": 83452101,
      "name": "astral-cli-v0.2.0-linux-x64.tar.gz",
      "content_type": "application/gzip",
      "size": 1048576,
      "browser_download_url": "https://github.com/The-Astral-Express-Family/astral-cli/releases/download/v0.2.0/astral-cli-v0.2.0-linux-x64.tar.gz"
    },
    {
      "url": "https://api.github.com/repos/The-Astral-Express-Family/astral-cli/releases/assets/83452102",
      "id": 83452102,
      "name": "astral-cli-v0.2.0-windows-x64.zip",
      "content_type": "application/zip",
      "size": 2097152,
      "browser_download_url": "https://github.com/The-Astral-Express-Family/astral-cli/releases/download/v0.2.0/astral-cli-v0.2.0-windows-x64.zip"
    }
  ],
  "body": "## Changes\n- first distribution build"
})json";

// Same schema as returned by GET /repos/<owner>/<repo>/releases/tags/<tag>.
const std::string kTaggedReleaseJson = R"json({
  "url": "https://api.github.com/repos/The-Astral-Express-Family/astral-cli/releases/196002233",
  "html_url": "https://github.com/The-Astral-Express-Family/astral-cli/releases/tag/v0.1.0",
  "assets_url": "https://api.github.com/repos/The-Astral-Express-Family/astral-cli/releases/196002233/assets",
  "tag_name": "v0.1.0",
  "target_commitish": "main",
  "name": "astral-cli v0.1.0",
  "draft": false,
  "prerelease": false,
  "created_at": "2026-08-10T08:00:00Z",
  "published_at": "2026-08-10T08:10:00Z",
  "assets": [
    {
      "url": "https://api.github.com/repos/The-Astral-Express-Family/astral-cli/releases/assets/82000001",
      "id": 82000001,
      "name": "astral-cli-v0.1.0-linux-x64.tar.gz",
      "content_type": "application/gzip",
      "size": 998644,
      "browser_download_url": "https://github.com/The-Astral-Express-Family/astral-cli/releases/download/v0.1.0/astral-cli-v0.1.0-linux-x64.tar.gz"
    }
  ],
  "body": "## Changes\n- initial release"
})json";

} // namespace

TEST_CASE("parseReleaseJson reads a releases/latest payload", "[github_release]") {
    const ReleaseInfo info = parseReleaseJson(kLatestReleaseJson);

    REQUIRE(info.tagName == "v0.2.0");
    REQUIRE(info.assets.size() == 2);
    REQUIRE(info.assets[0].name == "astral-cli-v0.2.0-linux-x64.tar.gz");
    REQUIRE(info.assets[0].downloadUrl ==
            "https://github.com/The-Astral-Express-Family/astral-cli/releases/download/v0.2.0/"
            "astral-cli-v0.2.0-linux-x64.tar.gz");
    REQUIRE(info.assets[1].name == "astral-cli-v0.2.0-windows-x64.zip");
    REQUIRE(info.assets[1].downloadUrl ==
            "https://github.com/The-Astral-Express-Family/astral-cli/releases/download/v0.2.0/"
            "astral-cli-v0.2.0-windows-x64.zip");
}

TEST_CASE("parseReleaseJson reads a releases/tags/<tag> payload", "[github_release]") {
    const ReleaseInfo info = parseReleaseJson(kTaggedReleaseJson);

    REQUIRE(info.tagName == "v0.1.0");
    REQUIRE(info.assets.size() == 1);
    REQUIRE(info.assets[0].name == "astral-cli-v0.1.0-linux-x64.tar.gz");
    REQUIRE(info.assets[0].downloadUrl ==
            "https://github.com/The-Astral-Express-Family/astral-cli/releases/download/v0.1.0/"
            "astral-cli-v0.1.0-linux-x64.tar.gz");
}

TEST_CASE("parseReleaseJson tolerates a release without assets", "[github_release]") {
    const ReleaseInfo info = parseReleaseJson(R"json({
      "tag_name": "v0.0.1",
      "assets": []
    })json");

    REQUIRE(info.tagName == "v0.0.1");
    REQUIRE(info.assets.empty());
}

TEST_CASE("parseReleaseJson rejects malformed JSON", "[github_release]") {
    try {
        parseReleaseJson("{not valid json");
        FAIL("expected AstralError for malformed JSON");
    } catch (const astral::core::AstralError& e) {
        REQUIRE(e.code() == astral::core::Errc::NetworkError);
    }
}

TEST_CASE("parseReleaseJson rejects a payload missing tag_name", "[github_release]") {
    REQUIRE_THROWS_AS(parseReleaseJson(R"({"assets": []})"), astral::core::AstralError);
}
