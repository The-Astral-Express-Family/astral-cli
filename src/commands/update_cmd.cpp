#include "commands/update_cmd.hpp"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>
#include <picosha2.h>

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>

#ifdef _WIN32
#include <process.h>
#define ASTRAL_GETPID _getpid
#else
#include <unistd.h>
#define ASTRAL_GETPID getpid
#endif

namespace fs = std::filesystem;
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

#include "client/github_release.hpp"
#include "client/http_client.hpp"
#include "core/distribution.hpp"
#include "core/error.hpp"
#include "core/exit_codes.hpp"
#include "core/semver.hpp"
#include "core/version.hpp"
#include "output/json_output.hpp"
#include "platform/archive.hpp"
#include "platform/self_update.hpp"
#include "platform/user_dirs.hpp"

namespace astral::commands {

namespace {

// 更新源：GitHub Releases（public 仓库，匿名访问）。
constexpr const char* kUpdateRepo = "The-Astral-Express-Family/astral-cli";
constexpr int kDownloadTimeoutSeconds = 300;

// 下载进度与目标版本对用户可见（human 模式），便于肉眼审计。

class UpdateCommand final : public Command {
public:
    const char* name() const override { return "update"; }
    const char* description() const override { return "Update the astral CLI binary"; }

    void configure(CLI::App& app) override {
        app.add_option("version", version_, "Target version, e.g. v0.2.1 (downgrades allowed)")
            ->check(CLI::Validator(
                [](std::string& value) {
                    if (!core::SemVer::parse(value)) {
                        throw CLI::ValidationError("version",
                                                    "invalid version format: " + value);
                    }
                    return std::string{};
                },
                "SEMVER", "semver"));
        app.add_flag("--check,--dry-run", checkOnly_, "Only report the latest version, do not install");
    }

    int execute(const CommandContext& context) override {
        client::HttpClient http;
        client::GithubReleaseClient releases(http);

        // 目标版本：显式参数（规范化为 v 前缀）或最新 release。
        std::string targetTag;
        if (!version_.empty()) {
            targetTag = version_;
            // SemVer::parse 接受 v/V 前缀；规范化时同样大小写不敏感地剥掉，
            // 避免 "V1.2.3" 被拼成 "vV1.2.3" 这种不存在的 tag。
            if (targetTag.front() == 'v' || targetTag.front() == 'V') {
                targetTag.erase(targetTag.begin());
            }
            targetTag = "v" + targetTag;
        }
        client::ReleaseInfo release = version_.empty() ? releases.latest(kUpdateRepo)
                                                       : releases.byTag(kUpdateRepo, targetTag);
        targetTag = release.tagName;

        const std::string current = core::versionString();

        if (checkOnly_) {
            if (context.json) {
                output::printJson(context.out, nlohmann::json{
                    {"command", "update"},
                    {"check", true},
                    {"from", current},
                    {"latest", targetTag},
                });
            } else {
                context.out << "current: " << current << ", latest: " << targetTag << "\n";
            }
            return 0;
        }

        // 已是目标版本：无事可做（含降级到当前版本的场景）。
        const auto currentVer = core::SemVer::parse(current);
        const auto targetVer = core::SemVer::parse(targetTag);
        if (currentVer && targetVer && *currentVer == *targetVer) {
            if (context.json) {
                output::printJson(context.out, nlohmann::json{
                    {"command", "update"},
                    {"from", current},
                    {"to", targetTag},
                });
            } else {
                context.out << "astral " << current << " is up to date\n";
            }
            return 0;
        }

        // 安装路径守卫：只允许自更新约定安装位置内的二进制。
        const fs::path exePath = platform::currentExecutable();
        platform::cleanupStaleUpdateFiles(exePath.parent_path());
        if (!platform::isManagedInstallLocation(exePath)) {
            throw core::AstralError(
                core::Errc::LocalWorkspaceError,
                "not installed via install script; download manually or run the install script");
        }

        if (!context.json) {
            context.err << "updating astral " << current << " -> " << targetTag << "\n";
        }

        // 临时工作目录：~/.astral-cli/tmp/update-<pid>/，作用域结束整目录清理。
        fs::path workDir;
        {
            std::error_code ec;
            workDir = platform::astralHome() / "tmp" /
                      ("update-" + std::to_string(static_cast<long>(ASTRAL_GETPID())));
            fs::remove_all(workDir, ec);
            fs::create_directories(workDir, ec);
            if (!fs::is_directory(workDir, ec)) {
                throw core::AstralError(core::Errc::LocalWorkspaceError,
                                        "cannot create update work dir " + workDir.string());
            }
        }
        struct DirGuard {
            fs::path dir;
            ~DirGuard() {
                std::error_code ec;
                fs::remove_all(dir, ec);
            }
        } dirGuard{workDir};

        // 资产选择：按平台映射拼名，绝不信 fixture 字符串。
        const auto assetName = core::assetNameFor(targetTag, core::buildPlatform());
        if (!assetName) {
            throw core::AstralError(core::Errc::LocalWorkspaceError,
                                    "unsupported platform for update: " + std::string(core::buildPlatform()));
        }
        std::string assetUrl;
        std::string sumsUrl;
        for (const auto& asset : release.assets) {
            if (asset.name == *assetName) {
                assetUrl = asset.downloadUrl;
            } else if (asset.name == "SHA256SUMS.txt") {
                sumsUrl = asset.downloadUrl;
            }
        }
        if (assetUrl.empty() || sumsUrl.empty()) {
            throw core::AstralError(core::Errc::ServerNotFound,
                                    "release " + targetTag + " has no asset " + *assetName +
                                        std::string(" or SHA256SUMS.txt"));
        }

        client::HttpClient::Options options;
        options.userAgent = std::string("astral-cli/") + core::kProjectVersion;
        options.requestTimeout = std::chrono::seconds(kDownloadTimeoutSeconds);
        options.followRedirects = true; // 资产下载 302 跳转到 objects.githubusercontent.com
        client::HttpClient downloader(options);

        const fs::path archivePath = workDir / *assetName;
        const fs::path sumsPath = workDir / "SHA256SUMS.txt";
        downloader.getToFile(assetUrl, archivePath);
        downloader.getToFile(sumsUrl, sumsPath);

        // 校验：逐行拆分 + trim（parseShaLine 不吃 \r\n），小写十六进制比对。
        std::ifstream sumsIn(sumsPath, std::ios::binary);
        if (!sumsIn) {
            throw core::AstralError(core::Errc::LocalWorkspaceError,
                                    "cannot read downloaded SHA256SUMS.txt");
        }
        std::optional<core::ShaSum> wanted;
        std::string line;
        while (std::getline(sumsIn, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
                                     line.back() == ' ' || line.back() == '\t')) {
                line.pop_back();
            }
            auto entry = core::parseShaLine(line);
            if (entry && entry->filename == *assetName) {
                wanted = entry;
                break;
            }
        }
        if (!wanted) {
            throw core::AstralError(core::Errc::UpdateIntegrity,
                                    "SHA256SUMS.txt has no entry for " + *assetName);
        }
        std::ifstream archiveIn(archivePath, std::ios::binary);
        if (!archiveIn) {
            throw core::AstralError(core::Errc::LocalWorkspaceError,
                                    "cannot read downloaded archive " + archivePath.string());
        }
        std::string actualHash;
        {
            std::string bytes((std::istreambuf_iterator<char>(archiveIn)),
                              std::istreambuf_iterator<char>());
            actualHash = picosha2::hash256_hex_string(bytes);
        }
        for (char& c : actualHash) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        std::string expectedHash = wanted->hex;
        for (char& c : expectedHash) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (actualHash != expectedHash) {
            throw core::AstralError(core::Errc::UpdateIntegrity,
                                    "checksum mismatch for " + *assetName + ": expected " +
                                        expectedHash + ", got " + actualHash);
        }

        // 提取 + 原子替换。
        std::string extractError;
        const auto extracted = platform::extractBinary(archivePath, workDir, extractError);
        if (!extracted) {
            throw core::AstralError(core::Errc::UpdateIntegrity,
                                    "cannot extract binary from " + *assetName + ": " +
                                        extractError);
        }
        std::string replaceError;
        if (!platform::replaceExecutable(exePath, *extracted, replaceError)) {
            throw core::AstralError(core::Errc::LocalWorkspaceError,
                                    "cannot replace " + exePath.string() + ": " + replaceError);
        }

        if (context.json) {
            output::printJson(context.out, nlohmann::json{
                {"command", "update"},
                {"from", current},
                {"to", targetTag},
            });
        } else {
            context.out << "astral " << current << " -> " << targetTag << " installed\n";
        }
        return 0;
    }

private:
    std::string version_;
    bool checkOnly_ = false;
};

} // namespace

std::unique_ptr<Command> makeUpdateCommand() {
    return std::make_unique<UpdateCommand>();
}

} // namespace astral::commands
