// core/content_hash 的向量级单测：sha256 契约口径（原始 UTF-8 bytes、
// "sha256:<64 小写 hex>" 前缀形状）、UTF-8 结构校验（超长编码/代理区/
// 截断序列必须拒）、fnv1a 跨进程稳定性形状。
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "core/content_hash.hpp"

namespace {

using astral::core::fnv1aHex;
using astral::core::isValidUtf8;
using astral::core::sha256ContentHash;

bool looksLikeProtocolHash(const std::string& value) {
    if (value.size() != 7 + 64 || value.rfind("sha256:", 0) != 0) {
        return false;
    }
    for (const char c : value.substr(7)) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("sha256 vectors pin the wire hash shape") {
    // 服务端 contentHash 同源实现：sha256.Sum256([]byte(...)) 小写 hex。
    CHECK(sha256ContentHash("") ==
          "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256ContentHash("abc") ==
          "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // NIST 例向量（>56 字节触发补块路径）。
    CHECK(sha256ContentHash("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "sha256:248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(sha256ContentHash("abc") != sha256ContentHash("abd"));
    CHECK(looksLikeProtocolHash(sha256ContentHash("x")));
}

TEST_CASE("hash covers raw bytes without newline rewriting") {
    // 协议红线（sync-semantics §7）：CRLF 必须原样进 hash，任何隐式换行
    // 重写都会让本地 hash 与服务端重算值分叉 → push 400。
    const std::string crlf = "line1\r\nline2\r\n";
    const std::string lf = "line1\nline2\n";
    CHECK(sha256ContentHash(crlf) != sha256ContentHash(lf));
    CHECK(isValidUtf8(crlf));
}

TEST_CASE("utf8 validation accepts text and rejects structural violations") {
    CHECK(isValidUtf8(""));
    CHECK(isValidUtf8("plain ascii"));
    CHECK(isValidUtf8("中文与 emoji 🎉"));
    CHECK(isValidUtf8("\xC3\xA9"));         // é (U+00E9)
    CHECK(isValidUtf8("\xE2\x82\xAC"));     // € (U+20AC)
    CHECK(isValidUtf8("\xF0\x9F\x8E\x89")); // 🎉 (U+1F389)

    CHECK_FALSE(isValidUtf8("\x80"));             // 裸 continuation
    CHECK_FALSE(isValidUtf8("\xC0\xAF"));         // 超长编码 '/'
    CHECK_FALSE(isValidUtf8("\xE0\x80\xAF"));     // 超长编码（3 字节）
    CHECK_FALSE(isValidUtf8("\xF0\x80\x80\xAF")); // 超长编码（4 字节）
    CHECK_FALSE(isValidUtf8("\xED\xA0\x80"));     // 代理区 U+D800
    CHECK_FALSE(isValidUtf8("\xF4\x90\x80\x80")); // U+110000 越界
    CHECK_FALSE(isValidUtf8("\xE2\x82"));         // 截断序列
    CHECK_FALSE(isValidUtf8("ok\xC3"));           // 尾部截断
}

TEST_CASE("fnv1a is deterministic for idempotency keys") {
    CHECK(fnv1aHex("a|b|c") == fnv1aHex("a|b|c"));
    CHECK(fnv1aHex("a|b|c") != fnv1aHex("a|b|d"));
    CHECK(fnv1aHex("").size() == 16);
}
