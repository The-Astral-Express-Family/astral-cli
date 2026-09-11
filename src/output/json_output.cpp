#include "output/json_output.hpp"

#include <ostream>

namespace astral::output {

nlohmann::json errorEnvelope(std::string_view code, std::string_view message,
                             const std::optional<std::string>& requestId,
                             const std::optional<bool>& retryable) {
    nlohmann::json error{
        {"code", code},
        {"message", message},
    };
    if (requestId) {
        error["request_id"] = *requestId;
    }
    if (retryable) {
        error["retryable"] = *retryable;
    }
    return nlohmann::json{{"error", std::move(error)}};
}

void printJson(std::ostream& out, const nlohmann::json& value) {
    out << value.dump() << '\n';
}

void printJsonError(std::ostream& out, std::string_view code, std::string_view message) {
    printJson(out, errorEnvelope(code, message));
}

} // namespace astral::output
