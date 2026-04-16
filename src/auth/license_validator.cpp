// ============================================================================
// ZeptoDB: License Validator Implementation
// ============================================================================
#include "zeptodb/auth/license_validator.h"
#include "zeptodb/auth/jwt_validator.h"
#include "zeptodb/common/logger.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>

namespace zeptodb::auth {

// ============================================================================
// Helpers
// ============================================================================
static std::string base64url_encode(const std::string& input) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    auto* d = reinterpret_cast<const unsigned char*>(input.data());
    size_t len = input.size();
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = static_cast<uint32_t>(d[i]) << 16;
        if (i + 1 < len) n |= static_cast<uint32_t>(d[i+1]) << 8;
        if (i + 2 < len) n |= static_cast<uint32_t>(d[i+2]);
        out += tbl[(n >> 18) & 63];
        out += tbl[(n >> 12) & 63];
        out += (i + 1 < len) ? tbl[(n >> 6) & 63] : '=';
        out += (i + 2 < len) ? tbl[n & 63]         : '=';
    }
    for (char& c : out) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    while (!out.empty() && out.back() == '=') out.pop_back();
    return out;
}

static bool get_json_bool(const std::string& json, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    auto pos = json.find(pat);
    if (pos == std::string::npos) return false;
    auto colon = json.find(':', pos + pat.size());
    if (colon == std::string::npos) return false;
    auto rest = json.substr(colon + 1, 10);
    return rest.find("true") != std::string::npos;
}

static Edition edition_from_string(const std::string& s) {
    if (s == "enterprise") return Edition::ENTERPRISE;
    // Backward compat: old "pro" license keys are treated as Enterprise (2-tier consolidation)
    if (s == "pro")        return Edition::ENTERPRISE;
    return Edition::COMMUNITY;
}

static const char* edition_to_string(Edition e) {
    switch (e) {
        case Edition::ENTERPRISE: return "Enterprise";
        default:                  return "Community";
    }
}

static int64_t now_unix() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

static std::string format_date(int64_t ts) {
    if (ts <= 0) return "never";
    std::time_t t = static_cast<std::time_t>(ts);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

// ============================================================================
// Constructor
// ============================================================================
LicenseValidator::LicenseValidator(std::string public_key_pem)
    : public_key_pem_(std::move(public_key_pem))
{
    if (public_key_pem_.empty())
        public_key_pem_ = DEFAULT_PUBLIC_KEY;
}

// ============================================================================
// load — try env, file, then direct string
// ============================================================================
bool LicenseValidator::load(const std::string& key_or_path) {
    std::string jwt;

    // 1. Environment variable
    if (const char* env = std::getenv("ZEPTODB_LICENSE_KEY"))
        jwt = env;

    // 2. License file
    if (jwt.empty()) {
        std::ifstream f("/etc/zeptodb/license.key");
        if (f.good()) {
            std::ostringstream ss;
            ss << f.rdbuf();
            jwt = ss.str();
            // Trim whitespace/newlines
            while (!jwt.empty() && (jwt.back() == '\n' || jwt.back() == '\r' || jwt.back() == ' '))
                jwt.pop_back();
        }
    }

    // 3. Direct string
    if (jwt.empty())
        jwt = key_or_path;

    if (jwt.empty()) {
        // No license — Community edition (silent)
        claims_ = LicenseClaims{};
        loaded_ = false;
        return false;
    }

    if (!decode_and_verify(jwt)) {
        ZEPTO_WARN("License key invalid — defaulting to Community edition");
        claims_ = LicenseClaims{};
        loaded_ = false;
        return false;
    }

    loaded_ = true;

    // Expiry warnings
    if (isExpired()) {
        if (inGracePeriod())
            ZEPTO_WARN("License expired — grace period active ({} days remaining)",
                       claims_.grace_days - static_cast<int>((now_unix() - claims_.expiry) / 86400));
        else
            ZEPTO_WARN("License expired and grace period ended — downgrading to Community");
    } else {
        int64_t days_left = (claims_.expiry - now_unix()) / 86400;
        if (days_left <= 7 && claims_.expiry > 0)
            ZEPTO_WARN("License expires in {} days", days_left);
    }

    return true;
}

// ============================================================================
// load_from_jwt_string_for_testing — skip signature verification
// ============================================================================
bool LicenseValidator::load_from_jwt_string_for_testing(const std::string& jwt) {
    if (jwt.empty()) {
        claims_ = LicenseClaims{};
        loaded_ = false;
        return false;
    }

    // Split into 3 parts
    auto p1 = jwt.find('.');
    if (p1 == std::string::npos) return false;
    auto p2 = jwt.find('.', p1 + 1);
    if (p2 == std::string::npos) return false;

    std::string b64_payload = jwt.substr(p1 + 1, p2 - p1 - 1);
    std::string payload = JwtValidator::base64url_decode(b64_payload);

    std::string ed = JwtValidator::get_json_string(payload, "edition");
    claims_.edition    = edition_from_string(ed);
    claims_.features   = static_cast<uint32_t>(JwtValidator::get_json_int64(payload, "features"));
    claims_.max_nodes  = static_cast<int>(JwtValidator::get_json_int64(payload, "max_nodes"));
    claims_.company    = JwtValidator::get_json_string(payload, "company");
    claims_.tenant_id  = JwtValidator::get_json_string(payload, "tenant_id");
    claims_.expiry     = JwtValidator::get_json_int64(payload, "exp");
    claims_.issued_at  = JwtValidator::get_json_int64(payload, "iat");

    int64_t gd = JwtValidator::get_json_int64(payload, "grace_days");
    claims_.grace_days = (gd > 0) ? static_cast<int>(gd) : 30;

    if (claims_.max_nodes <= 0) claims_.max_nodes = 1;

    // Trial detection
    trial_ = get_json_bool(payload, "trial");
    if (trial_) claims_.max_nodes = 1;

    loaded_ = true;
    return true;
}

// ============================================================================
// decode_and_verify — split JWT, verify RS256, extract claims
// ============================================================================
bool LicenseValidator::decode_and_verify(const std::string& jwt) {
    // Split into parts
    auto p1 = jwt.find('.');
    if (p1 == std::string::npos) return false;
    auto p2 = jwt.find('.', p1 + 1);
    if (p2 == std::string::npos) return false;

    std::string b64_header  = jwt.substr(0, p1);
    std::string b64_payload = jwt.substr(p1 + 1, p2 - p1 - 1);

    // Decode header to check for trial (alg:none)
    std::string header = JwtValidator::base64url_decode(b64_header);
    std::string payload = JwtValidator::base64url_decode(b64_payload);
    bool is_alg_none = JwtValidator::get_json_string(header, "alg") == "none";
    bool is_trial = get_json_bool(payload, "trial");

    if (is_alg_none && is_trial) {
        // Trial key: accept without signature, but enforce expiry
        int64_t exp = JwtValidator::get_json_int64(payload, "exp");
        if (exp > 0 && now_unix() > exp) {
            // Check grace period
            int64_t gd = JwtValidator::get_json_int64(payload, "grace_days");
            if (gd <= 0) gd = 30;
            if (now_unix() > exp + gd * 86400) return false;
        }
    } else {
        // Reject if no public key — cannot verify signature
        if (public_key_pem_.empty()) return false;

        std::string header_payload = jwt.substr(0, p2);
        JwtValidator::Config cfg;
        cfg.rs256_public_key_pem = public_key_pem_;
        cfg.verify_expiry = false;  // We handle expiry ourselves (grace period)
        JwtValidator v(cfg);
        auto result = v.validate(jwt);
        if (!result) return false;
    }

    // Decode payload and extract license claims
    std::string ed = JwtValidator::get_json_string(payload, "edition");
    claims_.edition    = edition_from_string(ed);
    claims_.features   = static_cast<uint32_t>(JwtValidator::get_json_int64(payload, "features"));
    claims_.max_nodes  = static_cast<int>(JwtValidator::get_json_int64(payload, "max_nodes"));
    claims_.company    = JwtValidator::get_json_string(payload, "company");
    claims_.tenant_id  = JwtValidator::get_json_string(payload, "tenant_id");
    claims_.expiry     = JwtValidator::get_json_int64(payload, "exp");
    claims_.issued_at  = JwtValidator::get_json_int64(payload, "iat");

    int64_t gd = JwtValidator::get_json_int64(payload, "grace_days");
    claims_.grace_days = (gd > 0) ? static_cast<int>(gd) : 30;

    if (claims_.max_nodes <= 0) claims_.max_nodes = 1;

    // Trial detection
    trial_ = is_trial;
    if (trial_) claims_.max_nodes = 1;

    return true;
}

// ============================================================================
// Query methods
// ============================================================================
bool LicenseValidator::hasFeature(Feature f) const {
    // Expired beyond grace → Community (no features)
    if (isExpired() && !inGracePeriod()) return false;
    return (claims_.features & static_cast<uint32_t>(f)) != 0;
}

bool LicenseValidator::isExpired() const {
    if (claims_.expiry <= 0) return false;  // No expiry set
    return now_unix() > claims_.expiry;
}

bool LicenseValidator::inGracePeriod() const {
    if (!isExpired()) return false;
    int64_t grace_end = claims_.expiry + static_cast<int64_t>(claims_.grace_days) * 86400;
    return now_unix() <= grace_end;
}

std::string LicenseValidator::statusLine() const {
    std::ostringstream ss;
    ss << "ZeptoDB v0.1.0 (" << edition_to_string(claims_.edition);
    if (trial_) ss << " Trial";
    if (loaded_ && !claims_.company.empty())
        ss << " — " << claims_.company;
    if (loaded_ && claims_.max_nodes > 1)
        ss << ", " << claims_.max_nodes << " nodes";
    if (loaded_ && claims_.expiry > 0)
        ss << ", expires " << format_date(claims_.expiry);
    ss << ")";
    return ss.str();
}

std::string LicenseValidator::startupBanner() const {
    std::string banner = statusLine();
    if (claims_.edition == Edition::COMMUNITY)
        banner += "\n  Upgrade to Enterprise: https://zeptodb.com/pricing";
    return banner;
}

// ============================================================================
// generate_trial_key — unsigned JWT with trial=true
// ============================================================================
std::string LicenseValidator::generate_trial_key(const std::string& company) {
    std::string header = base64url_encode(R"({"alg":"none","typ":"JWT"})");
    int64_t exp = now_unix() + 30 * 86400;
    std::string payload_json = R"({"edition":"enterprise","features":255,"max_nodes":1,"company":")"
        + company + R"(","exp":)" + std::to_string(exp)
        + R"(,"iat":)" + std::to_string(now_unix())
        + R"(,"trial":true})";
    std::string payload = base64url_encode(payload_json);
    return header + "." + payload + ".";
}

// ============================================================================
// Global singleton
// ============================================================================
LicenseValidator& license() {
    static LicenseValidator instance;
    return instance;
}

} // namespace zeptodb::auth
