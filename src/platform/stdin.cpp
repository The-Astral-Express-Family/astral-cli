#include "platform/stdin.hpp"

#include <cstdio>
#include <iostream>
#include <sstream>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>

#include "platform/win_headers.hpp"
#else
#include <termios.h>
#include <unistd.h>
#endif

namespace astral::platform {

std::string readStdinBinary() {
#ifdef _WIN32
    static const bool binary = [] {
        _setmode(_fileno(stdin), _O_BINARY);
        return true;
    }();
    (void)binary;
#endif
    std::ostringstream buffer;
    buffer << std::cin.rdbuf();
    return buffer.str();
}

bool stdinIsTty() {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(STDIN_FILENO) != 0;
#endif
}

std::string readLineNoEcho() {
    std::string line;
#ifdef _WIN32
    // 控制台行输入关掉 ENABLE_ECHO_INPUT 即可；重定向/管道下 GetConsoleMode
    // 失败，muted 保持 false，退化为普通 getline。
    HANDLE handle = GetStdHandle(STD_INPUT_HANDLE);
    DWORD original = 0;
    const bool muted =
        handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &original) != 0 &&
        SetConsoleMode(handle, original & ~static_cast<DWORD>(ENABLE_ECHO_INPUT)) != 0;
    std::getline(std::cin, line);
    if (muted) {
        SetConsoleMode(handle, original);
        std::cerr << '\n';
    }
#else
    struct termios original;
    bool muted = tcgetattr(STDIN_FILENO, &original) == 0;
    if (muted) {
        struct termios quiet = original;
        quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        muted = tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet) == 0;
    }
    std::getline(std::cin, line);
    if (muted) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
        std::cerr << '\n';
    }
#endif
    return line;
}

} // namespace astral::platform
