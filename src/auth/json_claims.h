#pragma once

// Small, strict JSON helpers for signed authentication claims.  These are
// intentionally limited to locating top-level object members and decoding the
// scalar/array types used by JWT and SSO claims.  Unlike substring-based
// extractors, they validate object structure, reject duplicate keys, and never
// scan forward into a later member for a value of the requested type.

#include <charconv>
#include <cctype>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace zeptodb::auth::detail {

enum class JsonFieldStatus {
    Missing,
    Valid,
    Invalid,
};

namespace json_claims_internal {

inline void skip_whitespace(const std::string& json, size_t* pos) {
    while (*pos < json.size() &&
           std::isspace(static_cast<unsigned char>(json[*pos]))) {
        ++*pos;
    }
}

inline int hex_value(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

inline bool read_hex4(const std::string& json, size_t* pos,
                      uint32_t* codepoint) {
    if (json.size() - *pos < 4) return false;
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i) {
        const int digit = hex_value(json[*pos + i]);
        if (digit < 0) return false;
        value = (value << 4U) | static_cast<uint32_t>(digit);
    }
    *pos += 4;
    *codepoint = value;
    return true;
}

inline bool append_utf8(uint32_t codepoint, std::string* output) {
    if (codepoint > 0x10ffffU ||
        (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
        return false;
    }
    if (!output) return true;
    if (codepoint <= 0x7fU) {
        output->push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ffU) {
        output->push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
        output->push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else if (codepoint <= 0xffffU) {
        output->push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
        output->push_back(static_cast<char>(
            0x80U | ((codepoint >> 6U) & 0x3fU)));
        output->push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else {
        output->push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
        output->push_back(static_cast<char>(
            0x80U | ((codepoint >> 12U) & 0x3fU)));
        output->push_back(static_cast<char>(
            0x80U | ((codepoint >> 6U) & 0x3fU)));
        output->push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    }
    return true;
}

inline bool parse_string(const std::string& json, size_t* pos,
                         std::string* output) {
    if (*pos >= json.size() || json[*pos] != '"') return false;
    ++*pos;
    if (output) output->clear();
    while (*pos < json.size()) {
        const unsigned char value =
            static_cast<unsigned char>(json[(*pos)++]);
        if (value == '"') return true;
        if (value < 0x20U) return false;
        if (value != '\\') {
            if (output) output->push_back(static_cast<char>(value));
            continue;
        }
        if (*pos >= json.size()) return false;
        const char escape = json[(*pos)++];
        switch (escape) {
            case '"':
            case '\\':
            case '/':
                if (output) output->push_back(escape);
                break;
            case 'b':
                if (output) output->push_back('\b');
                break;
            case 'f':
                if (output) output->push_back('\f');
                break;
            case 'n':
                if (output) output->push_back('\n');
                break;
            case 'r':
                if (output) output->push_back('\r');
                break;
            case 't':
                if (output) output->push_back('\t');
                break;
            case 'u': {
                uint32_t codepoint = 0;
                if (!read_hex4(json, pos, &codepoint)) return false;
                if (codepoint >= 0xd800U && codepoint <= 0xdbffU) {
                    if (json.size() - *pos < 6 || json[*pos] != '\\' ||
                        json[*pos + 1] != 'u') {
                        return false;
                    }
                    *pos += 2;
                    uint32_t low = 0;
                    if (!read_hex4(json, pos, &low) ||
                        low < 0xdc00U || low > 0xdfffU) {
                        return false;
                    }
                    codepoint = 0x10000U +
                        ((codepoint - 0xd800U) << 10U) +
                        (low - 0xdc00U);
                }
                if (!append_utf8(codepoint, output)) return false;
                break;
            }
            default:
                return false;
        }
    }
    return false;
}

inline bool skip_value(const std::string& json, size_t* pos,
                       size_t depth = 0);

inline bool skip_array(const std::string& json, size_t* pos, size_t depth) {
    if (depth > 64 || *pos >= json.size() || json[*pos] != '[') {
        return false;
    }
    ++*pos;
    skip_whitespace(json, pos);
    if (*pos < json.size() && json[*pos] == ']') {
        ++*pos;
        return true;
    }
    while (*pos < json.size()) {
        if (!skip_value(json, pos, depth + 1)) return false;
        skip_whitespace(json, pos);
        if (*pos < json.size() && json[*pos] == ',') {
            ++*pos;
            skip_whitespace(json, pos);
            continue;
        }
        if (*pos < json.size() && json[*pos] == ']') {
            ++*pos;
            return true;
        }
        return false;
    }
    return false;
}

inline bool skip_object(const std::string& json, size_t* pos, size_t depth) {
    if (depth > 64 || *pos >= json.size() || json[*pos] != '{') {
        return false;
    }
    ++*pos;
    skip_whitespace(json, pos);
    if (*pos < json.size() && json[*pos] == '}') {
        ++*pos;
        return true;
    }
    while (*pos < json.size()) {
        if (!parse_string(json, pos, nullptr)) return false;
        skip_whitespace(json, pos);
        if (*pos >= json.size() || json[*pos] != ':') return false;
        ++*pos;
        skip_whitespace(json, pos);
        if (!skip_value(json, pos, depth + 1)) return false;
        skip_whitespace(json, pos);
        if (*pos < json.size() && json[*pos] == ',') {
            ++*pos;
            skip_whitespace(json, pos);
            continue;
        }
        if (*pos < json.size() && json[*pos] == '}') {
            ++*pos;
            return true;
        }
        return false;
    }
    return false;
}

inline bool skip_number(const std::string& json, size_t* pos) {
    const size_t start = *pos;
    if (*pos < json.size() && json[*pos] == '-') ++*pos;
    if (*pos >= json.size()) return false;
    if (json[*pos] == '0') {
        ++*pos;
    } else if (json[*pos] >= '1' && json[*pos] <= '9') {
        while (*pos < json.size() &&
               std::isdigit(static_cast<unsigned char>(json[*pos]))) {
            ++*pos;
        }
    } else {
        return false;
    }
    if (*pos < json.size() && json[*pos] == '.') {
        ++*pos;
        const size_t fraction_start = *pos;
        while (*pos < json.size() &&
               std::isdigit(static_cast<unsigned char>(json[*pos]))) {
            ++*pos;
        }
        if (*pos == fraction_start) return false;
    }
    if (*pos < json.size() && (json[*pos] == 'e' || json[*pos] == 'E')) {
        ++*pos;
        if (*pos < json.size() &&
            (json[*pos] == '+' || json[*pos] == '-')) {
            ++*pos;
        }
        const size_t exponent_start = *pos;
        while (*pos < json.size() &&
               std::isdigit(static_cast<unsigned char>(json[*pos]))) {
            ++*pos;
        }
        if (*pos == exponent_start) return false;
    }
    return *pos > start;
}

inline bool consume_literal(const std::string& json, size_t* pos,
                            std::string_view literal) {
    if (json.compare(*pos, literal.size(), literal) != 0) return false;
    *pos += literal.size();
    return true;
}

inline bool skip_value(const std::string& json, size_t* pos, size_t depth) {
    if (depth > 64) return false;
    skip_whitespace(json, pos);
    if (*pos >= json.size()) return false;
    switch (json[*pos]) {
        case '"':
            return parse_string(json, pos, nullptr);
        case '{':
            return skip_object(json, pos, depth);
        case '[':
            return skip_array(json, pos, depth);
        case 't':
            return consume_literal(json, pos, "true");
        case 'f':
            return consume_literal(json, pos, "false");
        case 'n':
            return consume_literal(json, pos, "null");
        default:
            return skip_number(json, pos);
    }
}

struct MemberLocation {
    JsonFieldStatus status = JsonFieldStatus::Missing;
    size_t value_pos = 0;
};

inline MemberLocation find_member(const std::string& json,
                                  std::string_view requested_key) {
    size_t pos = 0;
    skip_whitespace(json, &pos);
    if (pos >= json.size() || json[pos] != '{') {
        return {JsonFieldStatus::Invalid, 0};
    }
    ++pos;
    skip_whitespace(json, &pos);

    bool found = false;
    size_t found_pos = 0;
    if (pos < json.size() && json[pos] == '}') {
        ++pos;
    } else {
        while (pos < json.size()) {
            std::string key;
            if (!parse_string(json, &pos, &key)) {
                return {JsonFieldStatus::Invalid, 0};
            }
            skip_whitespace(json, &pos);
            if (pos >= json.size() || json[pos] != ':') {
                return {JsonFieldStatus::Invalid, 0};
            }
            ++pos;
            skip_whitespace(json, &pos);
            const size_t value_pos = pos;
            if (!skip_value(json, &pos)) {
                return {JsonFieldStatus::Invalid, 0};
            }
            if (key == requested_key) {
                if (found) return {JsonFieldStatus::Invalid, 0};
                found = true;
                found_pos = value_pos;
            }
            skip_whitespace(json, &pos);
            if (pos < json.size() && json[pos] == ',') {
                ++pos;
                skip_whitespace(json, &pos);
                continue;
            }
            if (pos < json.size() && json[pos] == '}') {
                ++pos;
                break;
            }
            return {JsonFieldStatus::Invalid, 0};
        }
    }
    skip_whitespace(json, &pos);
    if (pos != json.size()) return {JsonFieldStatus::Invalid, 0};
    if (!found) return {JsonFieldStatus::Missing, 0};
    return {JsonFieldStatus::Valid, found_pos};
}

}  // namespace json_claims_internal

inline JsonFieldStatus read_json_string(const std::string& json,
                                        std::string_view key,
                                        std::string* output) {
    const auto member = json_claims_internal::find_member(json, key);
    if (member.status != JsonFieldStatus::Valid) return member.status;
    size_t pos = member.value_pos;
    if (!json_claims_internal::parse_string(json, &pos, output)) {
        return JsonFieldStatus::Invalid;
    }
    return JsonFieldStatus::Valid;
}

inline JsonFieldStatus read_json_int64(const std::string& json,
                                       std::string_view key,
                                       int64_t* output) {
    const auto member = json_claims_internal::find_member(json, key);
    if (member.status != JsonFieldStatus::Valid) return member.status;
    size_t end = member.value_pos;
    if (!json_claims_internal::skip_number(json, &end)) {
        return JsonFieldStatus::Invalid;
    }
    const auto* begin_ptr = json.data() + member.value_pos;
    const auto* end_ptr = json.data() + end;
    int64_t value = 0;
    const auto parsed = std::from_chars(begin_ptr, end_ptr, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end_ptr) {
        return JsonFieldStatus::Invalid;
    }
    *output = value;
    return JsonFieldStatus::Valid;
}

inline JsonFieldStatus read_json_bool(const std::string& json,
                                      std::string_view key,
                                      bool* output) {
    const auto member = json_claims_internal::find_member(json, key);
    if (member.status != JsonFieldStatus::Valid) return member.status;
    if (json.compare(member.value_pos, 4, "true") == 0) {
        *output = true;
        return JsonFieldStatus::Valid;
    }
    if (json.compare(member.value_pos, 5, "false") == 0) {
        *output = false;
        return JsonFieldStatus::Valid;
    }
    return JsonFieldStatus::Invalid;
}

inline JsonFieldStatus read_json_string_array(
    const std::string& json, std::string_view key,
    std::vector<std::string>* output) {
    const auto member = json_claims_internal::find_member(json, key);
    if (member.status != JsonFieldStatus::Valid) return member.status;
    size_t pos = member.value_pos;
    if (pos >= json.size() || json[pos] != '[') {
        return JsonFieldStatus::Invalid;
    }
    ++pos;
    json_claims_internal::skip_whitespace(json, &pos);
    std::vector<std::string> values;
    if (pos < json.size() && json[pos] == ']') {
        *output = std::move(values);
        return JsonFieldStatus::Valid;
    }
    while (pos < json.size()) {
        std::string value;
        if (!json_claims_internal::parse_string(json, &pos, &value)) {
            return JsonFieldStatus::Invalid;
        }
        values.push_back(std::move(value));
        json_claims_internal::skip_whitespace(json, &pos);
        if (pos < json.size() && json[pos] == ',') {
            ++pos;
            json_claims_internal::skip_whitespace(json, &pos);
            continue;
        }
        if (pos < json.size() && json[pos] == ']') {
            *output = std::move(values);
            return JsonFieldStatus::Valid;
        }
        return JsonFieldStatus::Invalid;
    }
    return JsonFieldStatus::Invalid;
}

}  // namespace zeptodb::auth::detail
