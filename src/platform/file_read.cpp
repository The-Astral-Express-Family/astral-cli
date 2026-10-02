#include "platform/file_read.hpp"

#include <fstream>

namespace astral::platform {

std::optional<std::string> readFileBinary(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    // 分块 append 而非 istreambuf_iterator 的 assign：gcc13 对后者的库内联
    // 路径有 -Wnull-dereference 误报（CI arm -Werror 红源），块状读取在
    // 任何流上都等价且无此告警面（AGENTS.md 同禁 istreambuf_iterator）。
    std::string content;
    char buffer[8192];
    while (input.read(buffer, sizeof buffer) || input.gcount() > 0) {
        content.append(buffer, static_cast<std::size_t>(input.gcount()));
    }
    return content;
}

} // namespace astral::platform
