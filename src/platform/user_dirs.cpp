#include "platform/user_dirs.hpp"

#include "core/env.hpp"

namespace fs = std::filesystem;

namespace astral::platform {

namespace {

std::string homeEnv() {
#ifdef _WIN32
    return core::env::get("USERPROFILE").value_or(core::env::get("HOME").value_or("."));
#else
    return core::env::get("HOME").value_or(".");
#endif
}

} // namespace

fs::path astralHomeFor(const std::string& home) {
    return fs::path(home) / ".astral-cli";
}

fs::path cacheDirFor(const std::string& home) {
    return astralHomeFor(home) / "cache";
}

fs::path astralHome() {
    if (auto custom = core::env::get("ASTRAL_HOME")) {
        return fs::path(*custom);
    }
    return astralHomeFor(homeEnv());
}

fs::path configDir() {
    return astralHome();
}

fs::path cacheDir() {
    return cacheDirFor(homeEnv());
}

} // namespace astral::platform
