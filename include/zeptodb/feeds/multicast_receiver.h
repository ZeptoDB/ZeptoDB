// ============================================================================
// ZeptoDB: Multicast UDP Receiver
// ============================================================================
#pragma once

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <cstdint>

namespace zeptodb::feeds {

// ============================================================================
// Multicast UDP Receiver (초저지연)
// ============================================================================
class MulticastReceiver {
public:
    using PacketCallback = std::function<void(const uint8_t*, size_t)>;

    MulticastReceiver(const std::string& group_ip,
                      uint16_t port,
                      const std::string& interface_ip = "0.0.0.0");
    ~MulticastReceiver();

    // 멀티캐스트 그룹 가입
    bool join();
    void leave();

    // 콜백 등록
    void on_packet(PacketCallback callback);

    // 수신 시작/중지
    void start();
    void stop();

    bool is_running() const { return running_.load(); }

    // 통계
    uint64_t get_packet_count() const { return packet_count_.load(); }
    uint64_t get_byte_count() const { return byte_count_.load(); }
    uint64_t get_error_count() const { return error_count_.load(); }

private:
    std::string group_ip_;
    uint16_t port_;
    std::string interface_ip_;

    int sockfd_;
    std::atomic<bool> running_;
    std::thread recv_thread_;

    PacketCallback callback_;

    // 통계
    std::atomic<uint64_t> packet_count_{0};
    std::atomic<uint64_t> byte_count_{0};
    std::atomic<uint64_t> error_count_{0};

    void recv_loop();
};

} // namespace zeptodb::feeds
