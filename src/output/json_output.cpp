#include "output/json_output.hpp"

#include <ostream>

namespace astral::output {

nlohmann::json errorEnvelope(std::string_view code, std::string_view message) {
    return nlohmann::json{
        {"error",
         {
             {"code", code},
             {"message", message},
         }},
    };
}

void printJson(std::ostream& out, const nlohmann::json& value) {
    out << value.dump() << '\n';
}

void printJsonError(std::ostream& out, std::string_view code, std::string_view message) {
    printJson(out, errorEnvelope(code, message));
}

} // namespace astral::output
