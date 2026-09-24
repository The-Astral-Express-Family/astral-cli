#include "platform/stdin.hpp"

#include <cstdio>
#include <iostream>
#include <sstream>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>

#include "platform/win_headers.hpp"
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

} // namespace astral::platform
