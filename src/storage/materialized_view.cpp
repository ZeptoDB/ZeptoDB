// ============================================================================
// ZeptoDB: Materialized View Manager — Implementation
// ============================================================================

#include "zeptodb/storage/materialized_view.h"
#include <algorithm>
#include <climits>

namespace zeptodb::storage {

bool MaterializedViewManager::create_view(MVDef def) {
    std::lock_guard lk(mu_);
    if (views_.count(def.view_name)) return false;
    ViewState vs;
    vs.def = std::move(def);
    views_[vs.def.view_name] = std::move(vs);
    return true;
}

bool MaterializedViewManager::drop_view(const std::string& name) {
    std::lock_guard lk(mu_);
    return views_.erase(name) > 0;
}

bool MaterializedViewManager::exists(const std::string& name) const {
    std::lock_guard lk(mu_);
    return views_.count(name) > 0;
}

void MaterializedViewManager::on_tick(int32_t symbol, int64_t ts,
                                       int64_t price, int64_t volume) {
    std::lock_guard lk(mu_);
    for (auto& [_, vs] : views_) {
        // Compute bucket key
        int64_t group_key = symbol;  // default: group by symbol
        int64_t time_key = 0;
        if (vs.def.xbar_bucket > 0) {
            time_key = (ts / vs.def.xbar_bucket) * vs.def.xbar_bucket;
        }

        ViewState::BucketKey bk{group_key, time_key};
        auto& bucket = vs.buckets[bk];

        // Initialize bucket on first access
        if (bucket.values.empty()) {
            bucket.group_key = group_key;
            bucket.time_key  = time_key;
            bucket.values.resize(vs.def.columns.size(), 0);
            bucket.counts.resize(vs.def.columns.size(), 0);
            // Initialize MIN to max, MAX to min
            for (size_t i = 0; i < vs.def.columns.size(); ++i) {
                if (vs.def.columns[i].agg == MVAggType::MIN)
                    bucket.values[i] = INT64_MAX;
                else if (vs.def.columns[i].agg == MVAggType::MAX)
                    bucket.values[i] = INT64_MIN;
            }
        }

        update_bucket(bucket, vs.def, price, volume);
    }
}

void MaterializedViewManager::update_bucket(MVBucket& bucket, const MVDef& def,
                                             int64_t price, int64_t volume) {
    for (size_t i = 0; i < def.columns.size(); ++i) {
        const auto& col = def.columns[i];
        int64_t src = 0;
        if (col.source_col == "price")       src = price;
        else if (col.source_col == "volume") src = volume;

        switch (col.agg) {
            case MVAggType::SUM:   bucket.values[i] += src; break;
            case MVAggType::COUNT: bucket.values[i] += 1;   break;
            case MVAggType::MIN:
                if (src < bucket.values[i]) bucket.values[i] = src;
                break;
            case MVAggType::MAX:
                if (src > bucket.values[i]) bucket.values[i] = src;
                break;
            case MVAggType::FIRST:
                if (bucket.counts[i] == 0) bucket.values[i] = src;
                break;
            case MVAggType::LAST:
                bucket.values[i] = src;
                break;
        }
        bucket.counts[i]++;
    }
}

MaterializedViewManager::ViewResult
MaterializedViewManager::query(const std::string& view_name) const {
    std::lock_guard lk(mu_);
    ViewResult result;

    auto it = views_.find(view_name);
    if (it == views_.end()) return result;

    const auto& vs = it->second;

    // Build column names
    result.column_names.push_back("symbol");
    if (vs.def.xbar_bucket > 0) result.column_names.push_back("bar");
    for (auto& col : vs.def.columns) result.column_names.push_back(col.name);

    // Build rows sorted by (group_key, time_key)
    std::vector<const MVBucket*> sorted;
    sorted.reserve(vs.buckets.size());
    for (auto& [_, b] : vs.buckets) sorted.push_back(&b);
    std::sort(sorted.begin(), sorted.end(), [](auto* a, auto* b) {
        return a->group_key < b->group_key ||
               (a->group_key == b->group_key && a->time_key < b->time_key);
    });

    for (auto* b : sorted) {
        std::vector<int64_t> row;
        row.push_back(b->group_key);
        if (vs.def.xbar_bucket > 0) row.push_back(b->time_key);
        for (auto& v : b->values) row.push_back(v);
        result.rows.push_back(std::move(row));
    }

    return result;
}

std::vector<std::string> MaterializedViewManager::list_views() const {
    std::lock_guard lk(mu_);
    std::vector<std::string> names;
    names.reserve(views_.size());
    for (auto& [name, _] : views_) names.push_back(name);
    return names;
}

} // namespace zeptodb::storage
