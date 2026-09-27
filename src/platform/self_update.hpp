#pragma once

#include <filesystem>
#include <string>

namespace astral::platform {

namespace fs = std::filesystem;

// 当前可执行文件绝对路径：Linux /proc/self/exe；macOS _NSGetExecutablePath+realpath；
// Windows GetModuleFileNameW。规格原文写 argv[0]，实施改用 OS API：argv[0] 可被
// 调用方伪造，OS API 才是规范来源（有意偏差，理由记录于此）。
fs::path currentExecutable();

// exe 位于 ~/.local/bin（POSIX，HOME 运行时读取）或
// %LOCALAPPDATA%\Programs\astral（Windows，LOCALAPPDATA 运行时读取）——
// 目录本身或其子目录 -> true。
bool isManagedInstallLocation(const fs::path& exePath);

// 原子替换：POSIX 先确保 replacement 为 0755 再 rename() 原子覆盖 target；
// Windows 先 MoveFileExW(target -> target.old, REPLACE_EXISTING) 再
// MoveFileExW(replacement -> target)。失败 -> false 且 error 说明。
// .old 残留清理由 cleanupStaleUpdateFiles 负责，不在此处。
bool replaceExecutable(const fs::path& target, const fs::path& replacement, std::string& error);

// 惰性清理：exeDir 下上次更新遗留的 astral.old / astral.exe.old；错误一律忽略。
void cleanupStaleUpdateFiles(const fs::path& exeDir);

} // namespace astral::platform
