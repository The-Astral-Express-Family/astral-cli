#include "platform/archive.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <cstdlib>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace astral::platform {

namespace {

// 目标成员名：平台相关的可执行文件名。
const char* wantedMemberName() {
#ifdef _WIN32
    return "astral.exe";
#else
    return "astral";
#endif
}

struct ArchivePtr {
    struct archive* handle = nullptr;

    ~ArchivePtr() {
        if (handle != nullptr) {
            archive_read_free(handle);
        }
    }
};

// 将当前条目的数据流式写入目标文件。
bool copyEntryData(struct archive* reader, const fs::path& dest, std::string& error) {
    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "cannot open destination file " + dest.string();
        return false;
    }

    const void* buffer = nullptr;
    size_t size = 0;
    la_int64_t offset = 0;
    for (;;) {
        const int readStatus = archive_read_data_block(reader, &buffer, &size, &offset);
        if (readStatus == ARCHIVE_EOF) {
            break;
        }
        if (readStatus != ARCHIVE_OK) {
            error = std::string("corrupt archive data: ") + archive_error_string(reader);
            return false;
        }
        out.write(static_cast<const char*>(buffer), static_cast<std::streamsize>(size));
        if (!out) {
            error = "failed writing extracted data to " + dest.string();
            return false;
        }
    }
    out.flush();
    if (!out) {
        // 最终缓冲未落盘（如磁盘满）也要视为失败。
        error = "failed flushing extracted data to " + dest.string();
        return false;
    }
    return true;
}

} // namespace

std::optional<fs::path> extractBinary(const fs::path& archive, const fs::path& destDir,
                                      std::string& error) {
    error.clear();

    std::error_code ec;
    fs::create_directories(destDir, ec);
    if (!fs::is_directory(destDir, ec)) {
        error = "cannot create destination directory " + destDir.string();
        return std::nullopt;
    }

    ArchivePtr guard{archive_read_new()};
    struct archive* reader = guard.handle;
    if (reader == nullptr) {
        error = "cannot allocate archive reader";
        return std::nullopt;
    }
    // 自动探测格式与压缩过滤器（tar.gz / zip 等）。
    archive_read_support_filter_all(reader);
    archive_read_support_format_all(reader);

    if (archive_read_open_filename(reader, archive.string().c_str(), 10240) != ARCHIVE_OK) {
        error = std::string("cannot open archive ") + archive.string() + ": " +
                archive_error_string(reader);
        return std::nullopt;
    }

    const char* wanted = wantedMemberName();
    for (;;) {
        struct archive_entry* entry = nullptr;
        const int headerStatus = archive_read_next_header(reader, &entry);
        if (headerStatus == ARCHIVE_EOF) {
            break;
        }
        if (headerStatus != ARCHIVE_OK) {
            error = std::string("corrupt archive header: ") + archive_error_string(reader);
            return std::nullopt;
        }
        if (archive_entry_filetype(entry) != AE_IFREG) {
            continue; // 目录、符号链接等一律跳过。
        }
        const char* pathname = archive_entry_pathname(entry);
        if (pathname == nullptr) {
            continue;
        }
        const fs::path memberPath(pathname);
        if (memberPath.filename() != wanted) {
            continue;
        }

        const fs::path dest = destDir / wanted;
        if (!copyEntryData(reader, dest, error)) {
            return std::nullopt;
        }
        // 0755：属主全权，组/其他可读+执行。Windows 上该调用无实义（忽略失败）。
        std::error_code permEc;
        fs::permissions(dest,
                        fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                            fs::perms::others_read | fs::perms::others_exec,
                        permEc);
        return dest;
    }

    error = std::string("archive does not contain the astral binary member");
    return std::nullopt;
}

} // namespace astral::platform
