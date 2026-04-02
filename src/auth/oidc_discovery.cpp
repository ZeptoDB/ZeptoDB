// ============================================================================
// ZeptoDB: OIDC Discovery Implementation
// ============================================================================
#include "zeptodb/auth/oidc_discovery.h"

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "third_party/httplib.h"

namespace zeptodb::auth {

std::string OidcDiscovery::extract_json_string(const std::string& json,
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

std::optional<OidcMetadata> OidcDiscovery::fetch(const std::string& issuer_url) {
    // Build well-known URL
    std::string base = issuer_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    std::string well_known = base + "/.well-known/openid-configuration";

    // Parse URL
    bool use_ssl = false;
    std::string url = well_known;
    if (url.substr(0, 8) == "https://") { use_ssl = true; url = url.substr(8); }
    else if (url.substr(0, 7) == "http://") { url = url.substr(7); }

    auto slash = url.find('/');
    std::string host_port = (slash != std::string::npos) ? url.substr(0, slash) : url;
    std::string path = (slash != std::string::npos) ? url.substr(slash) : "/";

    std::string body;
    if (use_ssl) {
        httplib::SSLClient cli(host_port);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(5);
        auto res = cli.Get(path);
        if (!res || res->status != 200) return std::nullopt;
        body = res->body;
    } else {
        httplib::Client cli(host_port);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(5);
        auto res = cli.Get(path);
        if (!res || res->status != 200) return std::nullopt;
        body = res->body;
    }

    OidcMetadata meta;
    meta.issuer                 = extract_json_string(body, "issuer");
    meta.jwks_uri               = extract_json_string(body, "jwks_uri");
    meta.authorization_endpoint = extract_json_string(body, "authorization_endpoint");
    meta.token_endpoint         = extract_json_string(body, "token_endpoint");
    meta.userinfo_endpoint      = extract_json_string(body, "userinfo_endpoint");

    // issuer and jwks_uri are required
    if (meta.issuer.empty() || meta.jwks_uri.empty()) return std::nullopt;

    return meta;
}

} // namespace zeptodb::auth
