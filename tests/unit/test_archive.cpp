#include <catch2/catch_test_macros.hpp>

#include <archive.h>
#include <archive_entry.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "platform/archive.hpp"

namespace fs = std::filesystem;
using astral::platform::extractBinary;

namespace {

fs::path makeTempDir() {
    static std::random_device device;
    static std::atomic<unsigned> counter{0};
    std::ostringstream name;
    name << "astral-archive-" << device() << "-" << counter++;
    const fs::path dir = fs::temp_directory_path() / name.str();
    fs::create_directories(dir);
    return dir;
}

std::string readFile(const fs::path& file) {
    std::ifstream input(file, std::ios::binary);
    // GCC 13 -O3 误报规避：istreambuf_iterator -> seekg/tellg/read。
    std::string bytes;
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    if (size > 0) {
        bytes.resize(static_cast<std::size_t>(size));
        input.seekg(0, std::ios::beg);
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        bytes.resize(static_cast<std::size_t>(input.gcount()));
    }
    return bytes;
}

struct Member {
    std::string path;
    std::string content;
};

// 测试内用 libarchive 写 API 现造 fixture，不依赖任何外部文件。
void writeArchive(const fs::path& file, bool zip, const std::vector<Member>& members) {
    struct archive* writer = archive_write_new();
    REQUIRE(writer != nullptr);
    if (zip) {
        REQUIRE(archive_write_set_format_zip(writer) == ARCHIVE_OK);
    } else {
        REQUIRE(archive_write_set_format_ustar(writer) == ARCHIVE_OK);
        REQUIRE(archive_write_add_filter_gzip(writer) == ARCHIVE_OK);
    }
    REQUIRE(archive_write_open_filename(writer, file.string().c_str()) == ARCHIVE_OK);

    for (const Member& member : members) {
        struct archive_entry* entry = archive_entry_new();
        REQUIRE(entry != nullptr);
        archive_entry_set_pathname(entry, member.path.c_str());
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        archive_entry_set_size(entry, static_cast<la_int64_t>(member.content.size()));
        REQUIRE(archive_write_header(writer, entry) == ARCHIVE_OK);
        const la_ssize_t written =
            archive_write_data(writer, member.content.data(), member.content.size());
        REQUIRE(written == static_cast<la_ssize_t>(member.content.size()));
        archive_entry_free(entry);
    }

    REQUIRE(archive_write_close(writer) == ARCHIVE_OK);
    REQUIRE(archive_write_free(writer) == ARCHIVE_OK);
}

std::string expectedMemberName() {
#ifdef _WIN32
    return "astral.exe";
#else
    return "astral";
#endif
}

std::size_t countFiles(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return 0;
    }
    std::size_t count = 0;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(dir)) {
        std::error_code fileEc;
        if (entry.is_regular_file(fileEc)) {
            ++count;
        }
    }
    return count;
}

} // namespace

TEST_CASE("extractBinary extracts the astral member from a tar.gz and skips the rest") {
    const fs::path dir = makeTempDir();
    const fs::path archivePath = dir / "update.tar.gz";
    writeArchive(archivePath, /*zip=*/false,
                 {{"dir/README.md", "release notes"}, {"dir/astral", "astral-payload-targz"}});

    const fs::path destDir = dir / "out";
    std::string error;
    const auto extracted = extractBinary(archivePath, destDir, error);

    REQUIRE(extracted.has_value());
    REQUIRE(*extracted == destDir / expectedMemberName());
    REQUIRE(readFile(*extracted) == "astral-payload-targz");
    REQUIRE_FALSE(fs::exists(destDir / "README.md"));
    REQUIRE_FALSE(fs::exists(destDir / "dir"));
    REQUIRE(countFiles(destDir) == 1);
    REQUIRE(error.empty());

#ifndef _WIN32
    std::error_code ec;
    const fs::perms perms = fs::status(*extracted, ec).permissions();
    const fs::perms executable = fs::perms::owner_all | fs::perms::group_read |
                                 fs::perms::group_exec | fs::perms::others_read |
                                 fs::perms::others_exec;
    REQUIRE((perms & fs::perms::mask) == executable);
#endif
}

TEST_CASE("extractBinary extracts the astral member from a zip") {
    const fs::path dir = makeTempDir();
    const fs::path archivePath = dir / "update.zip";
    writeArchive(archivePath, /*zip=*/true,
                 {{"bin/astral", "astral-payload-zip"}, {"bin/README.md", "notes"}});

    const fs::path destDir = dir / "out";
    std::string error;
    const auto extracted = extractBinary(archivePath, destDir, error);

    REQUIRE(extracted.has_value());
    REQUIRE(*extracted == destDir / expectedMemberName());
    REQUIRE(readFile(*extracted) == "astral-payload-zip");
    REQUIRE_FALSE(fs::exists(destDir / "README.md"));
    REQUIRE(countFiles(destDir) == 1);
    REQUIRE(error.empty());
}

TEST_CASE("extractBinary reports a missing astral member") {
    const fs::path dir = makeTempDir();
    const fs::path archivePath = dir / "no-member.tar.gz";
    writeArchive(archivePath, /*zip=*/false, {{"docs/notes.txt", "nothing else here"}});

    const fs::path destDir = dir / "out";
    std::string error;
    const auto extracted = extractBinary(archivePath, destDir, error);

    REQUIRE_FALSE(extracted.has_value());
    REQUIRE_FALSE(error.empty());
    REQUIRE_FALSE(fs::exists(destDir / expectedMemberName()));
}

TEST_CASE("extractBinary rejects a corrupt archive") {
    const fs::path dir = makeTempDir();
    const fs::path archivePath = dir / "broken.tar.gz";
    {
        std::ofstream output(archivePath, std::ios::binary);
        output << "definitely not an archive";
    }

    const fs::path destDir = dir / "out";
    std::string error;
    const auto extracted = extractBinary(archivePath, destDir, error);

    REQUIRE_FALSE(extracted.has_value());
    REQUIRE_FALSE(error.empty());
    REQUIRE(countFiles(destDir) == 0);
}
