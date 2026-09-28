#pragma once

// commands 层共享的渲染小件（todo/tags/msg 三家复用）：JSON 字段取值、
// UTF-8 截断、--json 列表页 envelope、人读分页尾注。此前逐字拷贝三份，
// 任何一份改动都会造成输出口径漂移，故收敛于此。

#include <cstddef>
#include <iosfwd>
#include <string>

#include <nlohmann/json.hpp>

namespace astral::output {

// String field of a JSON object; missing/null yields `fallback`, non-strings
// are dumped (numbers, bools).
std::string scalarOr(const nlohmann::json& object, const char* key,
                     const std::string& fallback = "");

// Cuts at a codepoint boundary so CJK text never gets mojibake'd.
std::string truncateUtf8(const std::string& text, std::size_t maxCodepoints);

// Machine envelope of a list page (ARCHITECTURE.md section 12):
// {"workspace_id": ..., "items": [...], "next_cursor": string|null}.
void printPageJson(std::ostream& out, const std::string& workspaceId, const nlohmann::json& items,
                   const std::string& nextCursor);

// Human footer when a page is truncated: "(N shown; more available - pass ...)".
// `flags` carries the command-specific hint (e.g. "--all or raise --limit").
void printMoreHint(std::ostream& out, std::size_t shown, const std::string& flags);

} // namespace astral::output
