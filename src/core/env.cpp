#include "core/env.hpp"

#include <cstdlib>

namespace astral::core::env {

std::optional<std::string> get(const std::string& name) {
    // MSVC 把 getenv 标成 C4996 deprecation，/WX 下直接编译失败；
    // 只读查询用 _dupenv_s 反而要手动 free，故局部关掉该警告。
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    const char* value = std::getenv(name.c_str());
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::string(value);
}

} // namespace astral::core::env
