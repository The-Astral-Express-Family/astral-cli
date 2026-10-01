#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace astral::platform {

// 从分发包（tar.gz 或 zip）中提取唯一的 astral 可执行成员，写到 destDir 下
// 同名文件并设置可执行权限（POSIX）。
//
// 成员匹配规则：条目 basename == "astral"（POSIX）或 "astral.exe"（Windows），
// 包内目录前缀任意；目录、符号链接及其他文件一律跳过。包内不含目标成员或
// 包损坏时返回 std::nullopt，并在 error 中给出可读的原因。
//
// 本函数只提取、绝不执行包内任何内容。
std::optional<std::filesystem::path> extractBinary(const std::filesystem::path& archive,
                                                   const std::filesystem::path& destDir,
                                                   std::string& error);

} // namespace astral::platform
