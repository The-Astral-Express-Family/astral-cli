#include "output/tty.hpp"

#include "core/env.hpp"  // getenv 收口在 core::env（MSVC 下 getenv 触发 C4996/-WX）

#include <string>

#ifdef _WIN32
// 同 browser.cpp：宏可能已被传递包含定义过，需守卫避免 -Werror 拒编。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace astral::output {

namespace {

bool fdIsTty(int fd) {
#ifdef _WIN32
    return _isatty(fd) != 0;
#else
    return isatty(fd) != 0;
#endif
}

bool noColorRequested() {
    // NO_COLOR 约定（no-color.org）：存在且非空即禁用颜色。
    return astral::core::env::get("NO_COLOR").has_value();
}

} // namespace

bool stdoutIsTty() {
    return fdIsTty(1);
}
bool stderrIsTty() {
    return fdIsTty(2);
}

void enableNativeAnsi() {
#ifdef _WIN32
    HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD mode = 0;
    if (GetConsoleMode(handle, &mode) != 0) {
        SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
    HANDLE errHandle = GetStdHandle(STD_ERROR_HANDLE);
    if (errHandle != INVALID_HANDLE_VALUE && GetConsoleMode(errHandle, &mode) != 0) {
        SetConsoleMode(errHandle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
#endif
}

bool shouldUseColor(const std::string& colorMode, bool jsonMode) {
    if (jsonMode) {
        return false;
    }
    if (colorMode == "always") {
        return true;
    }
    if (colorMode == "never") {
        return false;
    }
    return stdoutIsTty() && !noColorRequested();
}

} // namespace astral::output
