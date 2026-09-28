#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "core/error.hpp"

namespace astral::platform {
namespace fs = std::filesystem;

// 随机后缀（hex），用于临时文件名，避免并发写互踩。
std::string randomHexSuffix();

// 原子写：同目录临时文件 + rename。失败时清理临时文件并抛出
// core::AstralError(errc, ...)。ownerOnly 时尽力设置 0600（凭证类文件；
// Windows 上由用户 profile 目录 ACL 保护，set 位是 best-effort）。
void writeFileAtomic(const fs::path& target, std::string_view contents, core::Errc errc,
                     bool ownerOnly);

} // namespace astral::platform
