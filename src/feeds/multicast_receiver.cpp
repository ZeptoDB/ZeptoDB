// ============================================================================
// ZeptoDB: Multicast UDP Receiver Implementation
// ============================================================================
#include "zeptodb/feeds/multicast_receiver.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>

namespace zeptodb::feeds {

MulticastReceiver::MulticastReceiver(const std::string& group_ip,
                                     uint16_t port,
                                     const std::string& interface_ip)
    : group_ip_(group_ip)
    , port_(port)
    , interface_ip_(interface_ip)
    , sockfd_(-1)
    , running_(false)
{}

MulticastReceiver::~MulticastReceiver() {
    stop();
    leave();
}

bool MulticastReceiver::join() {
    // UDP 소켓 생성
    sockfd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd_ < 0) {
        return false;
    }

    // SO_REUSEADDR 설정
    int reuse = 1;
    if (setsockopt(sockfd_, SOL_SOCKET, SO_REUSEADDR,
                   &reuse, sizeof(reuse)) < 0) {
        close(sockfd_);
        sockfd_ = -1;
        return false;
    }

    // 로컬 주소 바인드
    struct sockaddr_in local_addr;
    std::memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(port_);
    local_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sockfd_, (struct sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
        close(sockfd_);
        sockfd_ = -1;
        return false;
    }

    // 멀티캐스트 그룹 가입
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(group_ip_.c_str());
    mreq.imr_interface.s_addr = inet_addr(interface_ip_.c_str());

    if (setsockopt(sockfd_, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   &mreq, sizeof(mreq)) < 0) {
        close(sockfd_);
        sockfd_ = -1;
        return false;
    }

    // 수신 버퍼 크기 증가 (패킷 손실 방지)
    int bufsize = 2 * 1024 * 1024;  // 2MB
    setsockopt(sockfd_, SOL_SOCKET, SO_RCVBUF,
               &bufsize, sizeof(bufsize));

    return true;
}

void MulticastReceiver::leave() {
    if (sockfd_ >= 0) {
        // 멀티캐스트 그룹 탈퇴
        struct ip_mreq mreq;
        mreq.imr_multiaddr.s_addr = inet_addr(group_ip_.c_str());
        mreq.imr_interface.s_addr = inet_addr(interface_ip_.c_str());
        setsockopt(sockfd_, IPPROTO_IP, IP_DROP_MEMBERSHIP,
                   &mreq, sizeof(mreq));

        close(sockfd_);
        sockfd_ = -1;
    }
}

void MulticastReceiver::on_packet(PacketCallback callback) {
    callback_ = callback;
}

void MulticastReceiver::start() {
    if (running_.load()) return;

    running_.store(true);
    recv_thread_ = std::thread(&MulticastReceiver::recv_loop, this);
}

void MulticastReceiver::stop() {
    if (!running_.load()) return;

    running_.store(false);

    if (recv_thread_.joinable()) {
        recv_thread_.join();
    }
}

void MulticastReceiver::recv_loop() {
    constexpr size_t MTU = 1500;  // Ethernet MTU
    uint8_t buffer[MTU];

    while (running_.load()) {
        ssize_t received = recvfrom(sockfd_, buffer, sizeof(buffer),
                                    0, nullptr, nullptr);

        if (received <= 0) {
            if (running_.load()) {
                error_count_.fetch_add(1);
            }
            continue;
        }

        packet_count_.fetch_add(1);
        byte_count_.fetch_add(received);

        // 콜백 호출 (초저지연 - 별도 복사 없음)
        if (callback_) {
            callback_(buffer, received);
        }
    }
}

} // namespace zeptodb::feeds
