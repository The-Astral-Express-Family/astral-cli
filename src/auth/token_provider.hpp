#pragma once

#include <optional>
#include <string>

#include "core/error.hpp"
#include "platform/credential_store.hpp"

namespace astral::auth {

// Token resolution policy (ARCHITECTURE.md section 6.3):
//   ASTRAL_TOKEN (process env) wins over the credentials file
//   (~/.astral-cli/credentials.json) and is never persisted; without
//   either, machine callers get AUTH_REQUIRED.
struct TokenRequest {
    std::optional<std::string> envToken;
    const platform::CredentialStore* store = nullptr;
    platform::ServerId server;
};

struct ResolvedToken {
    std::string token;
    std::string source; // "env" | "store"
};

// Returns nullopt when no credential is available for the server.
std::optional<ResolvedToken> resolveToken(const TokenRequest& request);

// Throwing variant used by network commands.
std::string requireToken(const TokenRequest& request);

} // namespace astral::auth
