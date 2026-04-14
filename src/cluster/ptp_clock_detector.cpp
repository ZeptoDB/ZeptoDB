#include "zeptodb/cluster/ptp_clock_detector.h"
#include "zeptodb/common/logger.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cmath>

namespace zeptodb::cluster {

PtpClockDetector::PtpClockDetector(const PtpConfig& cfg) : config_(cfg) {}

void PtpClockDetector::refresh() {
    bool avail = detect_ptp_device();
    ptp_available_.store(avail, std::memory_order_release);

    if (!avail) {
        status_.store(PtpSyncStatus::UNAVAILABLE, std::memory_order_release);
        offset_ns_.store(0, std::memory_order_release);
        return;
    }

    int64_t offset = read_ptp_offset();
    int64_t abs_offset = std::abs(offset);
    offset_ns_.store(abs_offset, std::memory_order_release);
    status_.store(classify_offset(abs_offset), std::memory_order_release);

    ZEPTO_DEBUG("PTP clock status: {} offset={}ns device={}",
                ptp_status_str(status()), abs_offset, ptp_device());
}

PtpSyncStatus PtpClockDetector::classify_offset(int64_t abs_offset_ns) const {
    if (abs_offset_ns <= config_.max_offset_ns)
        return PtpSyncStatus::SYNCED;
    if (abs_offset_ns <= config_.max_offset_ns * 10)
        return PtpSyncStatus::DEGRADED;
    return PtpSyncStatus::UNSYNC;
}

bool PtpClockDetector::allow_distributed_asof() const {
    if (!config_.strict_mode) return true;
    auto s = status();
    return s == PtpSyncStatus::SYNCED;
}

std::string PtpClockDetector::to_json() const {
    std::ostringstream os;
    os << "{\"ptp_available\":" << (ptp_available() ? "true" : "false")
       << ",\"status\":\"" << ptp_status_str(status()) << "\""
       << ",\"offset_ns\":" << offset_ns()
       << ",\"max_offset_ns\":" << config_.max_offset_ns
       << ",\"strict_mode\":" << (config_.strict_mode ? "true" : "false")
       << ",\"device\":\"" << ptp_device() << "\""
       << "}";
    return os.str();
}

std::string PtpClockDetector::ptp_device() const {
    std::lock_guard lock(mu_);
    return ptp_device_path_;
}

void PtpClockDetector::inject_offset(int64_t off_ns, bool avail) {
    ptp_available_.store(avail, std::memory_order_release);
    int64_t abs_off = std::abs(off_ns);
    offset_ns_.store(abs_off, std::memory_order_release);
    if (!avail) {
        status_.store(PtpSyncStatus::UNAVAILABLE, std::memory_order_release);
    } else {
        status_.store(classify_offset(abs_off), std::memory_order_release);
    }
}

bool PtpClockDetector::detect_ptp_device() {
    std::lock_guard lock(mu_);

    // Check /dev/ptp0, /dev/ptp1, ...
    for (int i = 0; i < 4; ++i) {
        std::string path = "/dev/ptp" + std::to_string(i);
        if (std::filesystem::exists(path)) {
            ptp_device_path_ = path;
            return true;
        }
    }

    // Check chrony tracking (chronyc tracking)
    if (std::filesystem::exists("/var/run/chrony") ||
        std::filesystem::exists("/run/chrony")) {
        ptp_device_path_ = "chrony";
        return true;
    }

    // Check if phc2sys is running (PTP hardware clock sync daemon)
    // Use pgrep to avoid shell injection
    if (std::system("pgrep -x phc2sys >/dev/null 2>&1") == 0) {
        ptp_device_path_ = "phc2sys";
        return true;
    }

    ptp_device_path_.clear();
    return false;
}

int64_t PtpClockDetector::read_ptp_offset() {
    // Try chronyc tracking output for "Last offset"
    // Format: "Last offset     : +0.000000123 seconds"
    FILE* fp = popen("chronyc tracking 2>/dev/null", "r");
    if (fp) {
        char buf[256];
        while (fgets(buf, sizeof(buf), fp)) {
            std::string line(buf);
            if (line.find("Last offset") != std::string::npos) {
                auto colon = line.find(':');
                if (colon != std::string::npos) {
                    std::string val = line.substr(colon + 1);
                    // Trim and parse float seconds
                    double secs = 0.0;
                    try { secs = std::stod(val); } catch (...) {}
                    pclose(fp);
                    return static_cast<int64_t>(secs * 1e9);
                }
            }
        }
        pclose(fp);
    }

    // Try phc_ctl output
    std::string dev;
    {
        std::lock_guard lock(mu_);
        dev = ptp_device_path_;
    }
    if (!dev.empty() && dev.rfind("/dev/ptp", 0) == 0) {
        std::string cmd = "phc_ctl " + dev + " -q get 2>/dev/null";
        fp = popen(cmd.c_str(), "r");
        if (fp) {
            char buf[256];
            if (fgets(buf, sizeof(buf), fp)) {
                // phc_ctl outputs offset in nanoseconds
                int64_t ns = 0;
                try { ns = std::stoll(std::string(buf)); } catch (...) {}
                pclose(fp);
                return ns;
            }
            pclose(fp);
        }
    }

    // Fallback: no offset measurable, report 0 (assume synced if device exists)
    return 0;
}

} // namespace zeptodb::cluster
