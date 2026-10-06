#pragma once

// 整文件二进制读取的共享实现：document --file、todo --tree-file、
// self-update 校验和三处原先各持一份（两份 8192 分块 + 一份 seekg/tellg），
// 现统一为分块读取。打不开/读失败返回 nullopt——错误语义（Usage 报
// 「cannot open」还是 LocalWorkspaceError 报下载档案损坏）由调用方定。

#include <filesystem>
#include <optional>
#include <string>

namespace astral::platform {

// 形参取 fs::path：self-update 传入含非 ASCII 目录的下载路径，string 窄化
// 在部分代码页下有损；std::string 调用点经隐式构造无损进入。
std::optional<std::string> readFileBinary(const std::filesystem::path& path);

} // namespace astral::platform
