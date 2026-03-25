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
        index_.emplace(std::move(key), code);
        return code;
    }

    /// Lookup string by code
    std::string_view lookup(uint32_t code) const {
        return code < strings_.size() ? std::string_view(strings_[code]) : "";
    }

    /// Find code by string (-1 if not found)
    int64_t find(std::string_view s) const {
        auto it = index_.find(std::string(s));
        return it != index_.end() ? static_cast<int64_t>(it->second) : -1;
    }

    size_t size() const { return strings_.size(); }

private:
    std::vector<std::string> strings_;
    std::unordered_map<std::string, uint32_t> index_;
};

} // namespace zeptodb::storage
