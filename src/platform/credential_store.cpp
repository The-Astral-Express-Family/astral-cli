#include "platform/credential_store.hpp"

#include <fstream>
#include <stdexcept>
#include <utility>

#include "platform/atomic_file.hpp"
#include "platform/user_dirs.hpp"

namespace astral::platform {

namespace {

// v2: 增加 sessions 槽（D12，human device-flow 登录态）。此前 login 未实装、
// 市面不存在 v1 文件，直接升版不做迁移。
constexpr int kCredentialsVersion = 2;

nlohmann::json freshRoot() {
    return nlohmann::json{{"version", kCredentialsVersion},
                          {"servers", nlohmann::json::object()},
                          {"sessions", nlohmann::json::object()}};
}

} // namespace

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

    // Owner-only：POSIX 上设 0600；Windows 文件留在用户 profile 内，由
    // 目录 ACL 保护，set 位在 helper 内是 best-effort。
    platform::writeFileAtomic(file_, root.dump(2) + '\n', core::Errc::CredentialStoreError,
                              /*ownerOnly=*/true);
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
    // tolerant：损坏/缺失的文件直接重建（shape 校验保证 servers 必为 object），
    // 所以 login 顺手就能修复坏文件。
    nlohmann::json root = readRoot(/*tolerant=*/true);
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
    root["servers"].erase(server);
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

namespace {

LoginSession sessionFromJson(const ServerId& key, const nlohmann::json& entry) {
    LoginSession session;
    session.serverUrl = key;
    session.serverId = entry.value("server_id", std::string());
    session.apiBase = entry.value("api_base", std::string());
    session.accessToken = entry.value("access_token", std::string());
    session.refreshToken = entry.value("refresh_token", std::string());
    session.principalId = entry.value("principal_id", std::string());
    return session;
}

nlohmann::json sessionToJson(const LoginSession& session) {
    return {
        {"server_id", session.serverId},       {"api_base", session.apiBase},
        {"access_token", session.accessToken}, {"refresh_token", session.refreshToken},
        {"principal_id", session.principalId},
    };
}

} // namespace

std::optional<LoginSession> FileCredentialStore::loadSession(const ServerId& serverUrl) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const nlohmann::json root = readRoot(/*tolerant=*/false);
    const auto sessions = root.find("sessions");
    if (sessions == root.end() || !sessions->is_object()) {
        return std::nullopt;
    }
    const auto entry = sessions->find(serverUrl);
    if (entry == sessions->end() || !entry->is_object()) {
        return std::nullopt;
    }
    return sessionFromJson(serverUrl, *entry);
}

void FileCredentialStore::saveSession(const ServerId& serverUrl, const LoginSession& session) {
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json root = readRoot(/*tolerant=*/true);
    root["sessions"][serverUrl] = sessionToJson(session);
    root["version"] = kCredentialsVersion;
    writeRoot(root);
}

void FileCredentialStore::eraseSession(const ServerId& serverUrl) {
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json root = readRoot(/*tolerant=*/true);
    root["sessions"].erase(serverUrl);
    writeRoot(root);
}

std::vector<ServerId> FileCredentialStore::listSessions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ServerId> urls;
    const nlohmann::json root = readRoot(/*tolerant=*/false);
    if (root.contains("sessions") && root["sessions"].is_object()) {
        for (auto it = root["sessions"].begin(); it != root["sessions"].end(); ++it) {
            urls.push_back(it.key());
        }
    }
    return urls;
}

std::unique_ptr<CredentialStore> makeDefaultCredentialStore() {
    return std::make_unique<FileCredentialStore>(astralHome() / "credentials.json");
}

} // namespace astral::platform
