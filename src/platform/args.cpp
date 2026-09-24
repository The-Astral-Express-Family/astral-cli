#include "platform/args.hpp"

#ifdef _WIN32
#include "core/content_hash.hpp"
#include "platform/win_headers.hpp"
#endif

namespace astral::platform {

#ifndef _WIN32

std::vector<std::string> argsToUtf8(int argc, char** argv) {
    return {argv, argv + argc};
}

#else

namespace {

std::string acpToUtf8(const std::string& text) {
    const int wideLength =
        MultiByteToWideChar(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (wideLength <= 0) {
        return text; // 无法解码（如代理对截断）：保留原样，后续 UTF-8 校验兜底
    }
    std::wstring wide(static_cast<std::size_t>(wideLength), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()), wide.data(),
                        wideLength);
    const int utf8Length =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLength, nullptr, 0, nullptr, nullptr);
    if (utf8Length <= 0) {
        return text;
    }
    std::string utf8(static_cast<std::size_t>(utf8Length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLength, utf8.data(), utf8Length, nullptr,
                        nullptr);
    return utf8;
}

} // namespace

std::vector<std::string> argsToUtf8(int argc, char** argv) {
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        std::string arg = argv[i];
        if (!core::isValidUtf8(arg)) {
            arg = acpToUtf8(arg);
        }
        args.push_back(std::move(arg));
    }
    return args;
}

#endif

} // namespace astral::platform
