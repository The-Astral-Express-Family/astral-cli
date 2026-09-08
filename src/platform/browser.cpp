#include "platform/browser.hpp"

#include <cstdlib>
#include <string_view>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace astral::platform {

bool openInBrowser(const std::string& url) {
#ifdef _WIN32
    HINSTANCE result = ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
#else
    std::string_view launcher;
#if defined(__APPLE__)
    launcher = "open";
#else
    launcher = "xdg-open";
#endif
    const std::string command = std::string(launcher) + " '" + url + "'";
    // launcher + URL are both controlled by us/discovery; no user input.
    const int status = std::system(command.c_str());
    return WIFEXITED(status) != 0 && WEXITSTATUS(status) == 0;
#endif
}

} // namespace astral::platform
