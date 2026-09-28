#pragma once

#include <string>
#include <string_view>

namespace astral::core {

// 文档 content_hash 的本地计算（协议契约：sha256:<64 小写 hex>，基于原始
// UTF-8 bytes，不重写换行——modulator TODO §1.3 标注的双端对接最易错点；
// 服务端以重算值为准，本地与远端口径不一致时 push 会被 400 拒绝）。
std::string sha256ContentHash(std::string_view bytes);

// Documents 只承诺 UTF-8 文本（FR-009）。读入的文件必须先过此校验再进
// JSON 序列化，否则 nlohmann::json 会在 dump 时才抛异常，且错误信息不含
// 路径与位置，Agent 无法定位坏文件。
bool isValidUtf8(std::string_view bytes);

// 确定性非加密散列（16 位十六进制），专用于 Idempotency-Key 派生：重跑同
// 一命令必须在跨进程/跨平台得到同一 key，服务端才能重放首次 2xx（msg
// send / document push 共用；std::hash 无此保证）。
std::string fnv1aHex(std::string_view text);

} // namespace astral::core
