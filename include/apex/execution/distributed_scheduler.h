#pragma once
// ============================================================================
// APEX-DB: DistributedQueryScheduler — TCP RPC 기반 분산 스케줄러
// ============================================================================
// QueryScheduler 인터페이스의 분산 구현체.
// scatter(): SQL 쿼리를 원격 노드에 TCP RPC로 전송, 결과를 PartialAggResult로 변환
// gather(): PartialAggResult 머지
// ============================================================================

#include "apex/execution/query_scheduler.h"
#include "apex/cluster/tcp_rpc.h"
#include <memory>
#include <string>
#include <vector>

namespace apex::execution {

class DistributedQueryScheduler : public QueryScheduler {
public:
    explicit DistributedQueryScheduler(
        std::vector<std::string> node_endpoints = {})
    {
        for (auto& ep : node_endpoints) {
            auto colon = ep.find(':');
            if (colon == std::string::npos) continue;
            std::string host = ep.substr(0, colon);
            uint16_t port = static_cast<uint16_t>(
                std::stoi(ep.substr(colon + 1)));
            clients_.push_back(
                std::make_unique<apex::cluster::TcpRpcClient>(host, port));
        }
    }

    std::vector<PartialAggResult> scatter(
        const std::vector<QueryFragment>& fragments) override
    {
        // Each fragment maps to a remote node; send the table_name as SQL
        std::vector<PartialAggResult> results;
        results.reserve(fragments.size());

        for (size_t i = 0; i < fragments.size() && i < clients_.size(); ++i) {
            // Build a simple SQL from the fragment
            std::string sql = "SELECT count(*) FROM " + fragments[i].table_name;
            auto qr = clients_[i]->execute_sql(sql);

            PartialAggResult par;
            par.rows_scanned = qr.rows_scanned;
            if (qr.ok() && !qr.rows.empty()) {
                par.resize(qr.rows[0].size());
                for (size_t c = 0; c < qr.rows[0].size(); ++c)
                    par.count[c] = qr.rows[0][c];
            }
            results.push_back(std::move(par));
        }
        return results;
    }

    PartialAggResult gather(
        std::vector<PartialAggResult>&& partials) override
    {
        if (partials.empty()) return {};
        PartialAggResult merged = std::move(partials[0]);
        for (size_t i = 1; i < partials.size(); ++i)
            merged.merge(partials[i]);
        return merged;
    }

    size_t worker_count() const override { return clients_.size(); }
    std::string scheduler_type() const override { return "distributed"; }

private:
    std::vector<std::unique_ptr<apex::cluster::TcpRpcClient>> clients_;
};

} // namespace apex::execution
