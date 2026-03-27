#pragma once
// ============================================================================
// Partition Migrator — stateful migration with checkpoint + retry
// ============================================================================
// Each Move goes through a state machine:
//   PENDING → DUAL_WRITE → COPYING → COMMITTED
//                             ↓
//                           FAILED (retryable)
//
// execute_plan() tracks per-move state. Failed moves are skipped (not abort
// entire plan). Caller can retry failed moves via resume_plan().
// ============================================================================

#include "zeptodb/cluster/partition_router.h"
#include "zeptodb/cluster/tcp_rpc.h"
#include "zeptodb/ingestion/tick_plant.h"

#include <atomic>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace zeptodb::cluster {

// ============================================================================
// MoveState: per-move state machine
// ============================================================================
enum class MoveState : uint8_t {
    PENDING,      // not started
    DUAL_WRITE,   // router dual-write enabled
    COPYING,      // data transfer in progress
    COMMITTED,    // data transferred + dual-write ended
    FAILED,       // error occurred (retryable from PENDING)
};

inline const char* move_state_str(MoveState s) {
    switch (s) {
        case MoveState::PENDING:    return "PENDING";
        case MoveState::DUAL_WRITE: return "DUAL_WRITE";
        case MoveState::COPYING:    return "COPYING";
        case MoveState::FAILED:     return "FAILED";
        case MoveState::COMMITTED:  return "COMMITTED";
    }
    return "???";
}

struct MoveStatus {
    PartitionRouter::Move move;
    MoveState             state = MoveState::PENDING;
    std::string           error;
    uint64_t              rows_migrated = 0;
    int                   attempts = 0;
};

// ============================================================================
// MigrationCheckpoint: in-memory plan state (resumable)
// ============================================================================
struct MigrationCheckpoint {
    std::vector<MoveStatus> moves;

    size_t pending_count() const {
        size_t n = 0;
        for (auto& m : moves)
            if (m.state == MoveState::PENDING || m.state == MoveState::FAILED) ++n;
        return n;
    }
    size_t committed_count() const {
        size_t n = 0;
        for (auto& m : moves) if (m.state == MoveState::COMMITTED) ++n;
        return n;
    }
    size_t failed_count() const {
        size_t n = 0;
        for (auto& m : moves) if (m.state == MoveState::FAILED) ++n;
        return n;
    }
    bool all_done() const { return pending_count() == 0; }

    /// Save checkpoint to JSON file. Returns true on success.
    bool save_to_file(const std::string& path) const {
        std::ofstream f(path);
        if (!f.is_open()) return false;
        f << "[\n";
        for (size_t i = 0; i < moves.size(); ++i) {
            auto& m = moves[i];
            f << "  {\"symbol\":" << m.move.symbol
              << ",\"from\":" << m.move.from
              << ",\"to\":" << m.move.to
              << ",\"state\":" << static_cast<int>(m.state)
              << ",\"error\":\"" << m.error << "\""
              << ",\"rows\":" << m.rows_migrated
              << ",\"attempts\":" << m.attempts
              << "}";
            if (i + 1 < moves.size()) f << ",";
            f << "\n";
        }
        f << "]\n";
        return f.good();
    }

    /// Load checkpoint from JSON file. Returns empty checkpoint on failure.
    static MigrationCheckpoint load_from_file(const std::string& path) {
        MigrationCheckpoint cp;
        std::ifstream f(path);
        if (!f.is_open()) return cp;

        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());

        // Minimal JSON array parser: extract each {...} object
        size_t pos = 0;
        while ((pos = content.find('{', pos)) != std::string::npos) {
            auto end = content.find('}', pos);
            if (end == std::string::npos) break;
            std::string obj = content.substr(pos, end - pos + 1);
            pos = end + 1;

            MoveStatus ms;
            ms.move.symbol = parse_int(obj, "\"symbol\":");
            ms.move.from   = static_cast<NodeId>(parse_int(obj, "\"from\":"));
            ms.move.to     = static_cast<NodeId>(parse_int(obj, "\"to\":"));
            ms.state       = static_cast<MoveState>(parse_int(obj, "\"state\":"));
            ms.rows_migrated = static_cast<uint64_t>(parse_int(obj, "\"rows\":"));
            ms.attempts    = static_cast<int>(parse_int(obj, "\"attempts\":"));

            // Parse error string
            auto err_pos = obj.find("\"error\":\"");
            if (err_pos != std::string::npos) {
                auto q1 = err_pos + 9;
                auto q2 = obj.find('"', q1);
                if (q2 != std::string::npos) ms.error = obj.substr(q1, q2 - q1);
            }
            cp.moves.push_back(std::move(ms));
        }
        return cp;
    }

private:
    static int64_t parse_int(const std::string& s, const std::string& key) {
        auto p = s.find(key);
        if (p == std::string::npos) return 0;
        p += key.size();
        while (p < s.size() && (s[p] == ' ' || s[p] == ':')) p++;
        bool neg = (p < s.size() && s[p] == '-');
        if (neg) p++;
        int64_t v = 0;
        while (p < s.size() && std::isdigit(s[p]))
            v = v * 10 + (s[p++] - '0');
        return neg ? -v : v;
    }
};

// ============================================================================
// MigrationStats
// ============================================================================
struct MigrationStats {
    std::atomic<uint64_t> moves_completed{0};
    std::atomic<uint64_t> moves_failed{0};
    std::atomic<uint64_t> rows_migrated{0};
};

// ============================================================================
// PartitionMigrator
// ============================================================================
class PartitionMigrator {
public:
    PartitionMigrator() = default;

    void add_node(NodeId id, const std::string& host, uint16_t port);

    /// Set max retry attempts per move (default 3).
    void set_max_retries(int n) { max_retries_ = n; }

    /// Set checkpoint file path. When set, checkpoint is saved to disk
    /// after each move completes (for crash recovery).
    void set_checkpoint_path(const std::string& path) { checkpoint_path_ = path; }
    const std::string& checkpoint_path() const { return checkpoint_path_; }

    /// Execute a migration plan with per-move state tracking.
    /// Returns a checkpoint with the final state of each move.
    /// Failed moves are retried up to max_retries_ times.
    MigrationCheckpoint execute_plan(const PartitionRouter::MigrationPlan& plan,
                                     PartitionRouter& router);

    /// Resume a previously failed plan. Retries PENDING and FAILED moves.
    /// Pass the checkpoint returned by execute_plan().
    MigrationCheckpoint resume_plan(MigrationCheckpoint checkpoint,
                                    PartitionRouter& router);

    /// Execute a single symbol migration (low-level).
    bool migrate_symbol(SymbolId symbol, NodeId from, NodeId to,
                        uint64_t& rows_out);

    const MigrationStats& stats() const { return stats_; }
    bool has_node(NodeId id) const { return nodes_.count(id) > 0; }

private:
    void execute_move(MoveStatus& ms, PartitionRouter& router);
    void rollback_move(SymbolId symbol, NodeId dest);

    std::unordered_map<NodeId, std::unique_ptr<TcpRpcClient>> nodes_;
    MigrationStats stats_;
    int max_retries_ = 3;
    std::string checkpoint_path_;
};

} // namespace zeptodb::cluster
