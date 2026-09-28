#pragma once

#include <string>
#include <vector>

namespace astral::platform {

// 把进程 argv 归一成 UTF-8（协议与 JSON 全线 UTF-8，nlohmann 对非法字节
// 直接抛 type_error.316）。
//
// Windows 的 argv 字节有两种来源：cmd.exe/IME 输入是 ANSI 代码页（中文
// 系统 = GBK），Git Bash / 管道 / 现代终端传入的本身就是 UTF-8。两者无法
// 从进程内可靠区分，采用 git/rustup 同款启发式：参数本身是合法 UTF-8 就
// 原样保留（幂等），否则按 ACP 解码再重编码为 UTF-8。极少数「恰为双重
// 合法」的 GBK 串会被按 UTF-8 保留——协议侧要的正是 UTF-8 解释。
// 其他平台 argv 本就是 UTF-8，等价透传（保持单一调用路径，无调用点分支）。
std::vector<std::string> argsToUtf8(int argc, char** argv);

} // namespace astral::platform
