#include "output/style.hpp"

#include <fmt/color.h>
#include <fmt/core.h>

namespace astral::output {

std::string Painter::ok(std::string_view text) const {
    return color_ ? fmt::format(fmt::emphasis::bold | fg(fmt::terminal_color::green), "{}", text)
                  : std::string(text);
}

std::string Painter::warn(std::string_view text) const {
    return color_ ? fmt::format(fg(fmt::terminal_color::yellow), "{}", text) : std::string(text);
}

std::string Painter::err(std::string_view text) const {
    return color_ ? fmt::format(fmt::emphasis::bold | fg(fmt::terminal_color::red), "{}", text)
                  : std::string(text);
}

std::string Painter::dim(std::string_view text) const {
    return color_ ? fmt::format(fmt::emphasis::faint, "{}", text) : std::string(text);
}

std::string Painter::bold(std::string_view text) const {
    return color_ ? fmt::format(fmt::emphasis::bold, "{}", text) : std::string(text);
}

std::string Painter::key(std::string_view text) const {
    return color_ ? fmt::format(fmt::emphasis::bold | fg(fmt::terminal_color::cyan), "{}", text)
                  : std::string(text);
}

} // namespace astral::output
