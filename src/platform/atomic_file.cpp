#include "platform/atomic_file.hpp"

#include <fstream>
#include <random>
#include <sstream>

namespace astral::platform {

std::string randomHexSuffix() {
    static std::random_device device;
    std::stringstream stream;
    stream << std::hex << device() << device();
    return stream.str();
}

void writeFileAtomic(const fs::path& target, std::string_view contents, core::Errc errc,
                     bool ownerOnly) {
    const fs::path temp =
        target.parent_path() / ("." + target.filename().string() + ".tmp." + randomHexSuffix());
    {
        std::ofstream output(temp, std::ios::binary | std::ios::trunc);
        if (!output) {
            std::error_code ec;
            fs::remove(temp, ec);
            throw core::AstralError(errc, "cannot write " + temp.string());
        }
        output << contents;
        output.flush();
        if (!output) {
            std::error_code ec;
            fs::remove(temp, ec);
            throw core::AstralError(errc, "failed writing " + temp.string());
        }
    }

    if (ownerOnly) {
        std::error_code ec;
        fs::permissions(temp, fs::perms::owner_read | fs::perms::owner_write,
                        fs::perm_options::replace, ec);
    }

    std::error_code ec;
    fs::rename(temp, target, ec);
    if (ec) {
        fs::remove(temp, ec);
        throw core::AstralError(errc, "cannot finalize " + target.string() + ": " + ec.message());
    }
}

} // namespace astral::platform
