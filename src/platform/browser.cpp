#include "platform/browser.hpp"

#include <string_view>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>  // ShellExecuteA：LEAN_AND_MEAN 不含 shell API
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
    // fork/exec 而非 system()：URL 来自服务器 discovery（半可信输入），
    // 不经过 shell 即无拼接/注入面。
    const pid_t pid = ::fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        // 子进程：exec 失败直接退出，绝不返回到调用方继续跑 CLI 逻辑。
        ::execlp(launcher.data(), launcher.data(), url.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) {
        return false;
    }
    return WIFEXITED(status) != 0 && WEXITSTATUS(status) == 0;
#endif
}

} // namespace astral::platform
