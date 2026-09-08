#pragma once

#include <string>
#include <string_view>

namespace astral::output {

// Minimal semantic styling for human output. When color is disabled every
// method returns its input unchanged, so call sites never branch on mode.
class Painter {
public:
    explicit Painter(bool colorEnabled) : color_(colorEnabled) {}

    bool enabled() const { return color_; }

    std::string ok(std::string_view text) const;
    std::string warn(std::string_view text) const;
    std::string err(std::string_view text) const;
    std::string dim(std::string_view text) const;
    std::string bold(std::string_view text) const;
    std::string key(std::string_view text) const;

private:
    bool color_;
};

} // namespace astral::output
