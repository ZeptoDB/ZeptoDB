// ============================================================================
// ZeptoDB: StringDictionary — Dictionary encoding for low-cardinality strings
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace zeptodb::storage {

class StringDictionary {
public:
    /// Intern a string: returns existing code or inserts new entry
    uint32_t intern(std::string_view s) {
        std::string key(s);
        auto it = index_.find(key);
        if (it != index_.end()) return it->second;
        uint32_t code = static_cast<uint32_t>(strings_.size());
        strings_.emplace_back(key);
        assigned_.push_back(true);
        index_.emplace(std::move(key), code);
        return code;
    }

    /// Ensure a replicated dictionary code maps to the expected string.
    /// Returns false if the code or string is already bound inconsistently.
    bool ensure_code(uint32_t code, std::string_view s) {
        std::string key(s);
        auto it = index_.find(key);
        if (it != index_.end()) return it->second == code;

        if (code >= strings_.size()) {
            strings_.resize(static_cast<size_t>(code) + 1);
            assigned_.resize(static_cast<size_t>(code) + 1, false);
        }
        if (assigned_[code] && strings_[code] != key) return false;

        strings_[code] = key;
        assigned_[code] = true;
        index_.emplace(strings_[code], code);
        return true;
    }

    /// Lookup string by code
    std::string_view lookup(uint32_t code) const {
        return code < strings_.size() && code < assigned_.size() && assigned_[code]
            ? std::string_view(strings_[code])
            : "";
    }

    /// Find code by string (-1 if not found)
    int64_t find(std::string_view s) const {
        auto it = index_.find(std::string(s));
        return it != index_.end() ? static_cast<int64_t>(it->second) : -1;
    }

    size_t size() const { return strings_.size(); }

private:
    std::vector<std::string> strings_;
    std::vector<bool> assigned_;
    std::unordered_map<std::string, uint32_t> index_;
};

} // namespace zeptodb::storage
