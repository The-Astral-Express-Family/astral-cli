#pragma once

#include <iosfwd>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace astral::output {

// Machine-output contract (ARCHITECTURE.md section 12):
//  - stdout receives exactly one JSON object (or one object per line for
//    streaming commands);
//  - no ANSI escapes;
//  - failures use the {"error":{"code","message"}} envelope;
//  - never emit token material.
void printJson(std::ostream& out, const nlohmann::json& value);
void printJsonError(std::ostream& out, std::string_view code, std::string_view message);
nlohmann::json errorEnvelope(std::string_view code, std::string_view message);

} // namespace astral::output
