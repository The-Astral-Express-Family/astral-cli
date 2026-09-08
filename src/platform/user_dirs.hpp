#pragma once

#include <filesystem>
#include <string>

namespace astral::platform {

namespace fs = std::filesystem;

// 用户级数据统一放在 ~/.astral-cli/（Windows: %USERPROFILE%\.astral-cli\）。
// 三端路径一致：凭证/配置/缓存都不再分散到 XDG / ~/Library / AppData。
fs::path astralHome();

// 纯函数变体，便于单测。
fs::path astralHomeFor(const std::string& home);
fs::path cacheDirFor(const std::string& home);

// 语义别名：configDir() == astralHome()。
fs::path configDir();
fs::path cacheDir();

} // namespace astral::platform
