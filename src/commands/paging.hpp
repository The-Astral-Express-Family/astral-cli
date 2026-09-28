// List-shaped commands (todo/msg/...) 的共享分页件：--limit/--all 旗标、
// query 串拼接。端点专属的过滤参数（如 task status/regex/tag）留在各命令，
// 经 appendParam 追加；新列表命令直接复用，不再手写第三份。
#pragma once

#include <optional>
#include <string>

#include <CLI/CLI.hpp>

#include "client/http_client.hpp"

namespace astral::commands {

// 追加 key=value（空值跳过；自动 & 前缀 + urlEncode）。
inline void appendParam(std::string& query, const std::string& key, const std::string& value) {
    if (value.empty()) {
        return;
    }
    if (!query.empty()) {
        query += '&';
    }
    query += key + '=' + client::urlEncode(value);
}

// 列表端点共享读旗标（--limit/--all）。
struct PageFlags {
    std::optional<int> limit;
    bool all = false;
};

inline void addPageFlags(CLI::App& app, PageFlags& flags,
                         const char* limitHelp = "Page size (server max 200)") {
    app.add_option("--limit", flags.limit, limitHelp);
    app.add_flag("--all", flags.all, "Follow next_cursor until exhausted");
}

// 把 PageFlags 对应的参数（limit）追加进 query（分隔符安全）。
inline void addPageParams(std::string& query, const PageFlags& flags) {
    if (flags.limit) {
        appendParam(query, "limit", std::to_string(*flags.limit));
    }
}

} // namespace astral::commands
