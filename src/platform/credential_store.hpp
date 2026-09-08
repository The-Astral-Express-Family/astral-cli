#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/error.hpp"

namespace astral::platform {

using ServerId = std::string;

struct Credential {
    std::string accessToken;
    std::string refreshToken;
    std::string principalId;
};

// 凭证存储统一接口（ARCHITECTURE.md 第 7 节）。默认实现是用户目录下的
// JSON 文件（~/.astral-cli/credentials.json）：跨平台路径一致、无 keyring
// 依赖、可 cat 可备份，可用性与开发便捷优先。
class CredentialStore {
public:
    virtual ~CredentialStore() = default;

    virtual std::string_view backendName() const = 0;
    virtual std::optional<Credential> load(const ServerId& server) const = 0;
    virtual void save(const ServerId& server, const Credential& credential) = 0;
    virtual void erase(const ServerId& server) = 0;
    // 已存凭证的 server_id 列表（doctor 展示用，不含任何 secret）。
    virtual std::vector<ServerId> list() const = 0;
};

// Ephemeral in-process store; used by tests.
class MemoryCredentialStore final : public CredentialStore {
public:
    std::string_view backendName() const override { return "memory"; }
    std::optional<Credential> load(const ServerId& server) const override;
    void save(const ServerId& server, const Credential& credential) override;
    void erase(const ServerId& server) override;
    std::vector<ServerId> list() const override;

private:
    std::map<ServerId, Credential> entries_;
};

// JSON 文件实现。格式（按 server_id 为主键，一文件多服务器）：
//   {"version": 1, "servers": {"srv_...": {"principal_id": "", "access_token":
//   "", "refresh_token": ""}}}
// 读写均原子（temp + rename），POSIX 上文件权限 0600。损坏文件时 load
// 报 CREDENTIAL_STORE_ERROR 并提示重新登录；save/erase 按空文件重建，
// 因此 `astral login` 可以直接修复损坏的凭证文件。
class FileCredentialStore final : public CredentialStore {
public:
    // file 传入完整文件路径（默认后端为 ~/.astral-cli/credentials.json）。
    explicit FileCredentialStore(std::filesystem::path file);

    std::string_view backendName() const override { return "file"; }
    const std::filesystem::path& path() const { return file_; }

    std::optional<Credential> load(const ServerId& server) const override;
    void save(const ServerId& server, const Credential& credential) override;
    void erase(const ServerId& server) override;
    std::vector<ServerId> list() const override;

private:
    nlohmann::json readRoot(bool tolerant) const;
    void writeRoot(const nlohmann::json& root) const;

    std::filesystem::path file_;
    mutable std::mutex mutex_;
};

// The default backend: ~/.astral-cli/credentials.json.
std::unique_ptr<CredentialStore> makeDefaultCredentialStore();

} // namespace astral::platform
