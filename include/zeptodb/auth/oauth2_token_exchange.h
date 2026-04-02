#pragma once
// ============================================================================
// ZeptoDB: OAuth2 Token Exchange
// ============================================================================
// Exchanges an authorization code for tokens at the IdP's token endpoint.
// Used in the OAuth2 Authorization Code Flow callback.
// ============================================================================

#include <optional>
#include <string>

namespace zeptodb::auth {

struct OAuth2Tokens {
    std::string access_token;
    std::string id_token;        // JWT — used for identity resolution
    std::string refresh_token;
    int64_t     expires_in = 0;  // seconds
};

struct OAuth2ExchangeParams {
    std::string token_endpoint;
    std::string code;
    std::string redirect_uri;
    std::string client_id;
    std::string client_secret;
};

class OAuth2TokenExchange {
public:
    /// Exchange authorization code for tokens.
    /// Returns nullopt on network error or non-200 response.
    static std::optional<OAuth2Tokens> exchange(const OAuth2ExchangeParams& params);

    /// Refresh an access token using a refresh token.
    static std::optional<OAuth2Tokens> refresh(const std::string& token_endpoint,
                                                const std::string& refresh_token,
                                                const std::string& client_id,
                                                const std::string& client_secret);

    static std::string extract_json_string(const std::string& json,
                                           const std::string& key);
    static int64_t extract_json_int(const std::string& json,
                                    const std::string& key);
};

} // namespace zeptodb::auth
