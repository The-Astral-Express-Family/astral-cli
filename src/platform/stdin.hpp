#pragma once

#include <string>

namespace astral::platform {

// 按字节原样读取全部 stdin（document push --file - 的内容来源）。Windows
// 的 stdin 默认文本模式会把 CRLF 折叠成 LF，破坏「hash 覆盖实际发送的
// bytes」这一协议前提，故先切二进制模式再读；其他平台无模式概念。
// 进程生命周期内首次调用生效一次，无需恢复（CLI 单命令即退出）。
std::string readStdinBinary();

// stdin 是否为交互终端。register 等命令据此决定缺失参数可否交互补问
// （非 TTY 时不等待键盘，直接按用法错误退出，机器调用不会被挂住）。
bool stdinIsTty();

// 读一行且不回显（密码输入；提示语由调用方先打印）。stdin 非 TTY 或平台
// 调用失败时退化为普通 getline（管道/脚本下本就无回显可言）。读毕恢复
// 终端回显并补一个换行——Enter 键本身没有被回显。
std::string readLineNoEcho();

} // namespace astral::platform
