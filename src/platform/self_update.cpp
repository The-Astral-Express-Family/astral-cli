#include "platform/self_update.hpp"

#include <optional>
#include <system_error>
#include <vector>

#include "core/env.hpp"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cstdlib> // realpath (POSIX)
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace fs = std::filesystem;

namespace astral::platform {

namespace {

// 受管安装目录：POSIX 为 $HOME/.local/bin，Windows 为 %LOCALAPPDATA%\Programs\astral。
// HOME/LOCALAPPDATA 运行时读取（可测试性）；未设置时返回 nullopt——这是自更新的
// 安全门，环境缺失必须保守拒绝（宁可 fail-safe，不做 CWD 相关的猜测）。
std::optional<fs::path> managedInstallRoot() {
#ifdef _WIN32
    const auto base = core::env::get("LOCALAPPDATA");
    if (!base) {
        return std::nullopt;
    }
    return fs::path(*base) / "Programs" / "astral";
#else
    const auto base = core::env::get("HOME");
    if (!base) {
        return std::nullopt;
    }
    return fs::path(*base) / ".local" / "bin";
#endif
}

} // namespace

fs::path currentExecutable() {
#if defined(_WIN32)
    DWORD size = MAX_PATH;
    std::wstring buffer(size, L'\0');
    for (;;) {
        const DWORD written = GetModuleFileNameW(nullptr, buffer.data(), size);
        if (written == 0) {
            return {};
        }
        if (written < size) {
            buffer.resize(written);
            return fs::path(buffer);
        }
        size *= 2; // 缓冲区不足，翻倍重试。
        buffer.resize(size);
    }
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size > 0 ? size : 1);
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return {};
    }
    if (char* resolved = ::realpath(buffer.data(), nullptr)) {
        fs::path result(resolved);
        ::free(resolved);
        return result;
    }
    return fs::path(buffer.data());
#else
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path{} : exe;
#endif
}

bool isManagedInstallLocation(const fs::path& exePath) {
    if (exePath.empty()) {
        return false;
    }
    std::error_code ec;
    // weakly_canonical：exe 与安装根都可能尚不存在（如 build 树内路径），做词法归一化后比较。
    const fs::path exe = fs::weakly_canonical(exePath, ec);
    if (ec) {
        return false;
    }
    const auto rootOpt = managedInstallRoot();
    if (!rootOpt) {
        return false; // 环境变量缺失：保守拒绝自更新。
    }
    const fs::path root = fs::weakly_canonical(*rootOpt, ec);
    if (ec) {
        return false;
    }
    auto exeIt = exe.begin();
    for (auto rootIt = root.begin(); rootIt != root.end(); ++rootIt, ++exeIt) {
        if (exeIt == exe.end() || *exeIt != *rootIt) {
            return false;
        }
    }
    return true; // 根目录本身或其子目录。
}

bool replaceExecutable(const fs::path& target, const fs::path& replacement, std::string& error) {
    error.clear();
#ifdef _WIN32
    // Windows 无原子覆盖：先挪走旧文件，再放新文件；失败尽力回滚。
    const std::wstring backup = target.wstring() + L".old";
    if (MoveFileExW(target.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING) == 0) {
        error = "replaceExecutable: failed to move existing file to .old (error " +
                std::to_string(GetLastError()) + ")";
        return false;
    }
    if (MoveFileExW(replacement.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING) == 0) {
        const DWORD code = GetLastError();
        MoveFileExW(backup.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING); // best-effort 回滚
        error = "replaceExecutable: failed to move replacement into place (error " +
                std::to_string(code) + ")";
        return false;
    }
    return true;
#else
    std::error_code ec;
    // 0755：owner rwx，group/others r-x。
    const fs::perms exePerms = fs::perms::owner_all | fs::perms::group_read |
                               fs::perms::group_exec | fs::perms::others_read |
                               fs::perms::others_exec;
    fs::permissions(replacement, exePerms, ec);
    if (ec) {
        error = "replaceExecutable: failed to set permissions on " + replacement.string() + ": " +
                ec.message();
        return false;
    }
    fs::rename(replacement, target, ec); // 同目录 rename() 原子覆盖。
    if (ec) {
        error = "replaceExecutable: failed to rename " + replacement.string() + " to " +
                target.string() + ": " + ec.message();
        return false;
    }
    return true;
#endif
}

void cleanupStaleUpdateFiles(const fs::path& exeDir) {
    std::error_code ec;
    fs::remove(exeDir / "astral.old", ec);
    fs::remove(exeDir / "astral.exe.old", ec);
}

} // namespace astral::platform
