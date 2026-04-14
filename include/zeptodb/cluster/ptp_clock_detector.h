#pragma once
// ============================================================================
// PtpClockDetector — PTP clock synchronization quality detection
// ============================================================================
// Checks system PTP (Precision Time Protocol) synchronization status for
// distributed ASOF JOIN strict mode.  When clock skew between nodes exceeds
// a configurable threshold, distributed ASOF JOINs are rejected to prevent
// inaccurate time-based matching.
//
// Sync status:
//   SYNCED      — PTP available, offset ≤ max_offset_ns
//   DEGRADED    — PTP available, offset > max_offset_ns but ≤ 10x threshold
//   UNSYNC      — PTP available, offset > 10x threshold
//   UNAVAILABLE — No PTP hardware or daemon detected
//
// Usage:
//   PtpClockDetector detector({.max_offset_ns = 1000});
//   detector.refresh();
//   if (detector.status() != PtpSyncStatus::SYNCED) { /* reject dist ASOF */ }
// ============================================================================

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace zeptodb::cluster {

// ============================================================================
// PtpSyncStatus
// ============================================================================
enum class PtpSyncStatus : uint8_t {
    SYNCED      = 0,  // offset within threshold
    DEGRADED    = 1,  // offset exceeds threshold but within 10x
    UNSYNC      = 2,  // offset far exceeds threshold
    UNAVAILABLE = 3,  // no PTP hardware/daemon found
};

inline const char* ptp_status_str(PtpSyncStatus s) {
    switch (s) {
        case PtpSyncStatus::SYNCED:      return "SYNCED";
        case PtpSyncStatus::DEGRADED:    return "DEGRADED";
        case PtpSyncStatus::UNSYNC:      return "UNSYNC";
        case PtpSyncStatus::UNAVAILABLE: return "UNAVAILABLE";
        default:                         return "???";
    }
}

// ============================================================================
// PtpConfig
// ============================================================================
struct PtpConfig {
    int64_t max_offset_ns       = 1000;   // 1μs default threshold
    bool    strict_mode         = false;   // reject distributed ASOF JOIN on bad sync
};

// ============================================================================
// PtpClockDetector
// ============================================================================
class PtpClockDetector {
public:
    explicit PtpClockDetector(const PtpConfig& cfg = {});

    /// Refresh PTP status by probing system.  Thread-safe.
    void refresh();

    /// Current sync status (last refresh result).
    PtpSyncStatus status() const { return status_.load(std::memory_order_acquire); }

    /// Last measured clock offset in nanoseconds (absolute value).
    int64_t offset_ns() const { return offset_ns_.load(std::memory_order_acquire); }

    /// Whether PTP hardware was detected on the system.
    bool ptp_available() const { return ptp_available_.load(std::memory_order_acquire); }

    /// Whether strict mode is enabled (reject distributed ASOF JOIN on bad sync).
    bool strict_mode() const { return config_.strict_mode; }
    void set_strict_mode(bool enabled) { config_.strict_mode = enabled; }

    /// Max offset threshold in nanoseconds.
    int64_t max_offset_ns() const { return config_.max_offset_ns; }
    void set_max_offset_ns(int64_t ns) { config_.max_offset_ns = ns; }

    /// Check if a distributed ASOF JOIN should be allowed.
    /// Returns true if allowed, false if strict mode blocks it.
    bool allow_distributed_asof() const;

    /// Build JSON status string for /admin/clock endpoint.
    std::string to_json() const;

    /// PTP device path detected (e.g. "/dev/ptp0"), empty if none.
    std::string ptp_device() const;

    // ---- Test injection ----

    /// Inject offset for testing (bypasses system probe).
    void inject_offset(int64_t offset_ns, bool ptp_available = true);

private:
    PtpSyncStatus classify_offset(int64_t abs_offset_ns) const;
    bool detect_ptp_device();
    int64_t read_ptp_offset();

    PtpConfig config_;
    std::atomic<PtpSyncStatus> status_{PtpSyncStatus::UNAVAILABLE};
    std::atomic<int64_t>       offset_ns_{0};
    std::atomic<bool>          ptp_available_{false};

    mutable std::mutex         mu_;
    std::string                ptp_device_path_;
};

} // namespace zeptodb::cluster
