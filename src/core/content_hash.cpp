#include "core/content_hash.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

#include <picosha2.h>

namespace astral::core {

std::string sha256ContentHash(std::string_view bytes) {
    std::vector<unsigned char> digest(picosha2::k_digest_size);
    picosha2::hash256(bytes.begin(), bytes.end(), digest.begin(), digest.end());
    return "sha256:" + picosha2::bytes_to_hex_string(digest.begin(), digest.end());
}

std::string fnv1aHex(std::string_view text) {
    std::uint64_t hash = 1469598103934665603ull;
    for (const unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
    return buffer;
}

// 结构校验（RFC 3629）：拒绝裸延续字节、非法起始、截断序列、代理区、
// 超长编码与 U+10FFFF 以上码点。只判形状不做任何规范化——是否重写换行/
// 编码由调用方决定（协议禁止隐式重写，hash 必须覆盖实际发送的 bytes）。
bool isValidUtf8(std::string_view bytes) {
    int remaining = 0; // 剩余待消费的 continuation 字节数
    int length = 0;    // 本序列总长（含首字节），完成时用于最小值校验
    unsigned int codepoint = 0;
    for (const unsigned char byte : bytes) {
        if (remaining == 0) {
            if (byte < 0x80) {
                continue;
            }
            if (byte >= 0xC2 && byte <= 0xDF) {
                remaining = 1;
                length = 2;
                codepoint = byte & 0x1F;
            } else if (byte >= 0xE0 && byte <= 0xEF) {
                remaining = 2;
                length = 3;
                codepoint = byte & 0x0F;
            } else if (byte >= 0xF0 && byte <= 0xF4) {
                remaining = 3;
                length = 4;
                codepoint = byte & 0x07;
            } else {
                return false; // 裸 continuation (0x80-0xBF) 或非法起始 (0xC0/0xC1/0xF5-0xFF)
            }
        } else {
            if ((byte & 0xC0) != 0x80) {
                return false;
            }
            codepoint = (codepoint << 6) | (byte & 0x3F);
            if (--remaining == 0) {
                const bool surrogate = codepoint >= 0xD800 && codepoint <= 0xDFFF;
                const unsigned int minimal = length == 2 ? 0x80 : length == 3 ? 0x800 : 0x10000;
                if (surrogate || codepoint < minimal || codepoint > 0x10FFFF) {
                    return false;
                }
            }
        }
    }
    return remaining == 0; // 截断的多字节序列
}

} // namespace astral::core
