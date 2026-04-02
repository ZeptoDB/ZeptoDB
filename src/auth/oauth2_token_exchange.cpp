// ============================================================================
// ZeptoDB: OAuth2 Token Exchange Implementation
// ============================================================================
#include "zeptodb/auth/oauth2_token_exchange.h"

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "third_party/httplib.h"

#include <sstream>

namespace zeptodb::auth {

std::string OAuth2TokenExchange::extract_json_string(const std::string& json,
                                                      const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return "";
    auto colon = json.find(':', pos + needle.size());
    if (colon == std::string::npos) return "";
    auto q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) return "";
    auto q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    return json.substr(q1 + 1, q2 - q1 - 1);
}

int64_t OAuth2TokenExchange::extract_json_int(const std::string& json,
                                               const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return 0;
    auto colon = json.find(':', pos + needle.size());
    if (colon == std::string::npos) return 0;
    auto p = colon + 1;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
    if (p >= json.size()) return 0;
    return std::strtoll(json.c_str() + p, nullptr, 10);
}

static std::optional<OAuth2Tokens> do_token_request(const std::string& token_endpoint,
                                                     const std::string& form_body) {
    // Parse URL
    std::string url = token_endpoint;
    bool use_ssl = false;
    if (url.substr(0, 8) == "https://") { use_ssl = true; url = url.substr(8); }
    else if (url.substr(0, 7) == "http://") { url = url.substr(7); }

    auto slash = url.find('/');
    std::string host_port = (slash != std::string::npos) ? url.substr(0, slash) : url;
    std::string path = (slash != std::string::npos) ? url.substr(slash) : "/";

    std::string body;
    if (use_ssl) {
        httplib::SSLClient cli(host_port);
        cli.set_connection_timeout(10);
        cli.set_read_timeout(10);
        auto res = cli.Post(path, form_body, "application/x-www-form-urlencoded");
        if (!res || res->status != 200) return std::nullopt;
        body = res->body;
    } else {
        httplib::Client cli(host_port);
        cli.set_connection_timeout(10);
        cli.set_read_timeout(10);
        auto res = cli.Post(path, form_body, "application/x-www-form-urlencoded");
        if (!res || res->status != 200) return std::nullopt;
        body = res->body;
    }

    OAuth2Tokens tokens;
    tokens.access_token  = OAuth2TokenExchange::extract_json_string(body, "access_token");
    tokens.id_token      = OAuth2TokenExchange::extract_json_string(body, "id_token");
    tokens.refresh_token = OAuth2TokenExchange::extract_json_string(body, "refresh_token");
    tokens.expires_in    = OAuth2TokenExchange::extract_json_int(body, "expires_in");

    if (tokens.access_token.empty() && tokens.id_token.empty())
        return std::nullopt;

    return tokens;
}

// URL-encode a string (minimal: spaces, &, =, +, etc.)
static std::string url_encode(const std::string& s) {
    std::ostringstream os;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            os << c;
        else
            os << '%' << std::uppercase << std::hex
               << ((c >> 4) & 0xF) << (c & 0xF);
    }
    return os.str();
}

std::optional<OAuth2Tokens> OAuth2TokenExchange::exchange(
    const OAuth2ExchangeParams& params)
{
    std::ostringstream form;
    form << "grant_type=authorization_code"
         << "&code=" << url_encode(params.code)
         << "&redirect_uri=" << url_encode(params.redirect_uri)
         << "&client_id=" << url_encode(params.client_id);
    if (!params.client_secret.empty())
        form << "&client_secret=" << url_encode(params.client_secret);

    return do_token_request(params.token_endpoint, form.str());
}

std::optional<OAuth2Tokens> OAuth2TokenExchange::refresh(
    const std::string& token_endpoint,
    const std::string& refresh_token,
    const std::string& client_id,
    const std::string& client_secret)
{
    std::ostringstream form;
    form << "grant_type=refresh_token"
         << "&refresh_token=" << url_encode(refresh_token)
         << "&client_id=" << url_encode(client_id);
    if (!client_secret.empty())
        form << "&client_secret=" << url_encode(client_secret);

    return do_token_request(token_endpoint, form.str());
}

} // namespace zeptodb::auth
