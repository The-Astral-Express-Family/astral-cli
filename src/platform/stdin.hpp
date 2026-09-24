#pragma once

#include <string>

namespace astral::platform {

// 按字节原样读取全部 stdin（document push --file - 的内容来源）。Windows
// 的 stdin 默认文本模式会把 CRLF 折叠成 LF，破坏「hash 覆盖实际发送的
// bytes」这一协议前提，故先切二进制模式再读；其他平台无模式概念。
// 进程生命周期内首次调用生效一次，无需恢复（CLI 单命令即退出）。
std::string readStdinBinary();

} // namespace astral::platform
