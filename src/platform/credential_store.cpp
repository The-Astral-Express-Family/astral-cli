#include "platform/credential_store.hpp"

#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "platform/user_dirs.hpp"

namespace astral::platform {

namespace {

constexpr int kCredentialsVersion = 1;

nlohmann::json freshRoot() {
    return nlohmann::json{{"version", kCredentialsVersion}, {"servers", nlohmann::json::object()}};
}

std::string randomSuffix() {
    static std::random_device device;
    std::stringstream stream;
    stream << std::hex << device();
    return stream.str();
}

} // namespace

std::optional<Credential> MemoryCredentialStore::load(const ServerId& server) const {
    auto it = entries_.find(server);
    if (it == entries_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void MemoryCredentialStore::save(const ServerId& server, const Credential& credential) {
    entries_.insert_or_assign(server, credential);
}

void MemoryCredentialStore::erase(const ServerId& server) {
    entries_.erase(server);
}

std::vector<ServerId> MemoryCredentialStore::list() const {
    std::vector<ServerId> servers;
    servers.reserve(entries_.size());
    for (const auto& [server, credential] : entries_) {
        (void)credential;
        servers.push_back(server);
    }
    return servers;
}

FileCredentialStore::FileCredentialStore(std::filesystem::path file) : file_(std::move(file)) {}

nlohmann::json FileCredentialStore::readRoot(bool tolerant) const {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(file_, ec)) {
        return freshRoot();
    }

    std::ifstream input(file_);
    if (!input) {
        if (tolerant) {
            return freshRoot();
        }
        throw core::AstralError(core::Errc::CredentialStoreError, "cannot read " + file_.string());
    }

    try {
        nlohmann::json root = nlohmann::json::parse(input);
        const auto version = root.find("version");
        const auto servers = root.find("servers");
        const bool shapeOk = version != root.end() && version->is_number_integer() &&
                             version->get<int>() == kCredentialsVersion && servers != root.end() &&
                             servers->is_object();
        if (!shapeOk) {
            throw std::runtime_error("unexpected credentials file shape");
        }
        return root;
    } catch (const std::exception&) {
        if (tolerant) {
            return freshRoot();
        }
        throw core::AstralError(core::Errc::CredentialStoreError,
                                "credentials file is malformed: " + file_.string() +
                                    " (re-run `astral login` to rewrite it, or delete the file)");
    }
}

void FileCredentialStore::writeRoot(const nlohmann::json& root) const {
    namespace fs = std::filesystem;
    const fs::path dir = file_.parent_path();
    if (!dir.empty()) {
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) {
            throw core::AstralError(core::Errc::CredentialStoreError,
                                    "cannot create " + dir.string() + ": " + ec.message());
        }
    }

    const fs::path temp = dir / ("." + file_.filename().string() + ".tmp." + randomSuffix());
    {
        std::ofstream output(temp, std::ios::binary | std::ios::trunc);
        if (!output) {
            fs::remove(temp);
            throw core::AstralError(core::Errc::CredentialStoreError,
                                    "cannot write " + temp.string());
        }
        output << root.dump(2) << '\n';
        output.flush();
        if (!output) {
            fs::remove(temp);
            throw core::AstralError(core::Errc::CredentialStoreError,
                                    "failed writing " + temp.string());
        }
    }

    // Owner-only：POSIX 上设 0600；Windows 文件留在用户 profile 内，由
    // 目录 ACL 保护，这里 set 位是 best-effort。
    std::error_code ec;
    fs::permissions(temp, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace,
                    ec);

    fs::rename(temp, file_, ec);
    if (ec) {
        fs::remove(temp, ec);
        throw core::AstralError(core::Errc::CredentialStoreError,
                                "cannot finalize " + file_.string() + ": " + ec.message());
    }
}

std::optional<Credential> FileCredentialStore::load(const ServerId& server) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const nlohmann::json root = readRoot(/*tolerant=*/false);
    const auto servers = root.find("servers");
    if (servers == root.end() || !servers->is_object()) {
        return std::nullopt;
    }
    const auto entry = servers->find(server);
    if (entry == servers->end() || !entry->is_object()) {
        return std::nullopt;
    }
    Credential credential;
    credential.accessToken = entry->value("access_token", std::string());
    credential.refreshToken = entry->value("refresh_token", std::string());
    credential.principalId = entry->value("principal_id", std::string());
    return credential;
}

void FileCredentialStore::save(const ServerId& server, const Credential& credential) {
    std::lock_guard<std::mutex> lock(mutex_);
    // tolerant：损坏/缺失的文件直接重建，所以 login 顺手就能修复坏文件。
    nlohmann::json root = readRoot(/*tolerant=*/true);
    if (!root.contains("servers") || !root["servers"].is_object()) {
        root["servers"] = nlohmann::json::object();
    }
    root["servers"][server] = {
        {"principal_id", credential.principalId},
        {"access_token", credential.accessToken},
        {"refresh_token", credential.refreshToken},
    };
    root["version"] = kCredentialsVersion;
    writeRoot(root);
}

void FileCredentialStore::erase(const ServerId& server) {
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json root = readRoot(/*tolerant=*/true);
    if (root.contains("servers") && root["servers"].is_object()) {
        root["servers"].erase(server);
    }
    writeRoot(root);
}

std::vector<ServerId> FileCredentialStore::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ServerId> servers;
    const nlohmann::json root = readRoot(/*tolerant=*/false);
    if (root.contains("servers") && root["servers"].is_object()) {
        for (auto it = root["servers"].begin(); it != root["servers"].end(); ++it) {
            servers.push_back(it.key());
        }
    }
    return servers;
}

std::unique_ptr<CredentialStore> makeDefaultCredentialStore() {
    return std::make_unique<FileCredentialStore>(astralHome() / "credentials.json");
}

} // namespace astral::platform
