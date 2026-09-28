#include "output/render.hpp"

#include <ostream>
#include <utility>

#include "output/json_output.hpp"

namespace astral::output {

std::string scalarOr(const nlohmann::json& object, const char* key, const std::string& fallback) {
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) {
        return fallback;
    }
    if (it->is_string()) {
        return it->get<std::string>();
    }
    return it->dump();
}

std::string truncateUtf8(const std::string& text, std::size_t maxCodepoints) {
    std::size_t codepoints = 0;
    std::size_t offset = 0;
    while (offset < text.size()) {
        if (codepoints == maxCodepoints) {
            return text.substr(0, offset) + "...";
        }
        const unsigned char lead = static_cast<unsigned char>(text[offset]);
        std::size_t len = 1;
        if ((lead & 0xE0) == 0xC0) {
            len = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            len = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            len = 4;
        }
        offset += len;
        ++codepoints;
    }
    return text;
}

void printPageJson(std::ostream& out, const std::string& workspaceId, const nlohmann::json& items,
                   const std::string& nextCursor) {
    printJson(out, {{"workspace_id", workspaceId},
                    {"items", items},
                    {"next_cursor",
                     nextCursor.empty() ? nlohmann::json(nullptr) : nlohmann::json(nextCursor)}});
}

void printMoreHint(std::ostream& out, std::size_t shown, const std::string& flags) {
    out << "(" << shown << " shown; more available - pass " << flags << ")\n";
}

} // namespace astral::output
