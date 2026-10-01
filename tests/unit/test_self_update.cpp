#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "core/env.hpp"
#include "platform/self_update.hpp"

// 注意：HOME 变异用例修改进程环境，catch_discover_tests 逐进程串行执行；
// 本文件不可与其他依赖 HOME 的测试并行（当前按二进制整体运行，天然串行）。

namespace fs = std::filesystem;

using astral::platform::cleanupStaleUpdateFiles;
using astral::platform::currentExecutable;
using astral::platform::isManagedInstallLocation;
using astral::platform::replaceExecutable;

namespace {

void setEnv(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    ::setenv(name, value.c_str(), 1);
#endif
}

// 环境守卫：构造时保存 HOME（Windows 另存 LOCALAPPDATA），析构时恢复。
class HomeGuard {
public:
    HomeGuard() {
        hadHome_ = readEnv("HOME", home_);
#ifdef _WIN32
        hadLocal_ = readEnv("LOCALAPPDATA", local_);
#endif
    }

    ~HomeGuard() {
        if (hadHome_) {
            setEnv("HOME", home_);
        }
#ifdef _WIN32
        if (hadLocal_) {
            setEnv("LOCALAPPDATA", local_);
        }
#endif
    }

    HomeGuard(const HomeGuard&) = delete;
    HomeGuard& operator=(const HomeGuard&) = delete;

private:
    static bool readEnv(const char* name, std::string& out) {
        // MSVC 对 std::getenv 有 C4996 弃用告警（CI /WX 判死）；
        // 统一走项目的 core::env 抽象（Windows 侧宽字符转换）。
        if (auto value = astral::core::env::get(name)) {
            out = *value;
            return true;
        }
        return false;
    }

    bool hadHome_ = false;
    std::string home_;
#ifdef _WIN32
    bool hadLocal_ = false;
    std::string local_;
#endif
};

fs::path makeTempDir(const char* tag) {
    const fs::path dir = fs::temp_directory_path() / (std::string("astral-selfupdate-") + tag);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void writeFile(const fs::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    out << content;
}

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    // GCC 13 -O3 误报规避：istreambuf_iterator -> seekg/tellg/read。
    std::string bytes;
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    if (size > 0) {
        bytes.resize(static_cast<std::size_t>(size));
        in.seekg(0, std::ios::beg);
        in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        bytes.resize(static_cast<std::size_t>(in.gcount()));
    }
    return bytes;
}

} // namespace

TEST_CASE("currentExecutable returns an existing absolute path") {
    const fs::path exe = currentExecutable();
    REQUIRE(exe.is_absolute());
    REQUIRE(fs::exists(exe));
}

#ifndef _WIN32
TEST_CASE("exe under HOME/.local/bin is a managed install location") {
    HomeGuard guard;
    const fs::path home = makeTempDir("home-managed");
    setEnv("HOME", home.string());

    const fs::path binDir = home / ".local" / "bin";
    fs::create_directories(binDir);
    const fs::path exe = binDir / "astral";
    writeFile(exe, "x");

    REQUIRE(isManagedInstallLocation(exe));
}

TEST_CASE("exe in a subdirectory of HOME/.local/bin is managed") {
    HomeGuard guard;
    const fs::path home = makeTempDir("home-subdir");
    setEnv("HOME", home.string());

    const fs::path subDir = home / ".local" / "bin" / "sub";
    fs::create_directories(subDir);
    const fs::path exe = subDir / "astral";
    writeFile(exe, "x");

    REQUIRE(isManagedInstallLocation(exe));
}

#endif // !_WIN32

TEST_CASE("build tree exe is not a managed install location") {
    HomeGuard guard;
    const fs::path home = makeTempDir("home-build");
    setEnv("HOME", home.string());

    const fs::path buildExe = fs::current_path() / "build" / "dev" / "astral";
    REQUIRE_FALSE(isManagedInstallLocation(buildExe));
}

TEST_CASE("unrelated temp path is not a managed install location") {
    HomeGuard guard;
    const fs::path home = makeTempDir("home-unrelated");
    setEnv("HOME", home.string());

    const fs::path unrelated = makeTempDir("unrelated") / "astral";
    writeFile(unrelated, "x");
    REQUIRE_FALSE(isManagedInstallLocation(unrelated));
}

#ifdef _WIN32
TEST_CASE("exe under LOCALAPPDATA/Programs/astral is a managed install location") {
    HomeGuard guard;
    const fs::path home = makeTempDir("win-localappdata");
    setEnv("LOCALAPPDATA", home.string());

    const fs::path appDir = home / "Programs" / "astral";
    fs::create_directories(appDir);
    const fs::path exe = appDir / "astral.exe";
    writeFile(exe, "x");

    REQUIRE(isManagedInstallLocation(exe));
}
#endif

TEST_CASE("replaceExecutable swaps content atomically and leaves no .new leftover") {
    const fs::path dir = makeTempDir("replace");
    const fs::path target = dir / "astral";
    const fs::path replacement = dir / "astral.new";
    writeFile(target, "old-binary");
    writeFile(replacement, "new-binary");

    std::string error;
    REQUIRE(replaceExecutable(target, replacement, error));
    REQUIRE(error.empty());
    REQUIRE(readFile(target) == "new-binary");
    REQUIRE_FALSE(fs::exists(replacement));
}

TEST_CASE("replaceExecutable reports an error for a missing replacement") {
    const fs::path dir = makeTempDir("replace-missing");
    const fs::path target = dir / "astral";
    writeFile(target, "old-binary");
    const fs::path replacement = dir / "does-not-exist.new";

    std::string error;
    REQUIRE_FALSE(replaceExecutable(target, replacement, error));
    REQUIRE_FALSE(error.empty());
    // 失败时 target 保持原内容。
    REQUIRE(readFile(target) == "old-binary");
}

TEST_CASE("cleanupStaleUpdateFiles removes leftover .old file") {
    const fs::path dir = makeTempDir("cleanup");
    const fs::path stale = dir / "astral.old";
    writeFile(stale, "stale-binary");

    cleanupStaleUpdateFiles(dir);

    REQUIRE_FALSE(fs::exists(stale));
}

TEST_CASE("cleanupStaleUpdateFiles ignores absent leftovers without error") {
    const fs::path dir = makeTempDir("cleanup-empty");
    cleanupStaleUpdateFiles(dir);
    REQUIRE(fs::exists(dir));
}
