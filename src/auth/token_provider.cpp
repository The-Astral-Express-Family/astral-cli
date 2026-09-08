#include "auth/token_provider.hpp"

namespace astral::auth {

std::optional<ResolvedToken> resolveToken(const TokenRequest& request) {
    if (request.envToken && !request.envToken->empty()) {
        return ResolvedToken{*request.envToken, "env"};
    }
    if (request.store != nullptr) {
        if (auto credential = request.store->load(request.server); credential) {
            return ResolvedToken{credential->accessToken, "store"};
        }
    }
    return std::nullopt;
}

std::string requireToken(const TokenRequest& request) {
    if (auto token = resolveToken(request)) {
        return token->token;
    }
    throw core::AstralError(
        core::Errc::AuthRequired,
        "no credential for this server: run `astral login` or set ASTRAL_TOKEN");
}

} // namespace astral::auth
