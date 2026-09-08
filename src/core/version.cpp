#include "core/version.hpp"

namespace astral::core {

const char* buildPlatform() {
#if defined(_WIN64)
    return "windows/x86_64";
#elif defined(_WIN32)
    return "windows/x86";
#elif defined(__APPLE__) && (defined(__aarch64__) || defined(__ARM64__))
    return "macos/arm64";
#elif defined(__APPLE__)
    return "macos/x86_64";
#elif defined(__linux__) && defined(__aarch64__)
    return "linux/arm64";
#elif defined(__linux__)
    return "linux/x86_64";
#else
    return "unknown";
#endif
}

} // namespace astral::core
