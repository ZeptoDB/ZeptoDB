// ============================================================================
// ZeptoDB: FIX Feed Handler Implementation
// ============================================================================
#include "zeptodb/feeds/fix_feed_handler.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <chrono>

namespace zeptodb::feeds {

FIXFeedHandler::FIXFeedHandler(const FeedConfig& config, SymbolMapper* mapper)
    : config_(config)
    , mapper_(mapper)
    , builder_(config.username, "TARGET")  // username을 SenderCompID로 사용
    , sockfd_(-1)
    , status_(FeedStatus::DISCONNECTED)
    , running_(false)
{}

FIXFeedHandler::~FIXFeedHandler() {
    disconnect();
}

bool FIXFeedHandler::connect() {
    if (is_connected()) return true;

    status_.store(FeedStatus::CONNECTING);

    if (!connect_socket()) {
        status_.store(FeedStatus::ERROR);
        notify_error("Failed to connect socket");
        return false;
    }

    if (!send_logon()) {
        close_socket();
        status_.store(FeedStatus::ERROR);
        notify_error("Failed to send logon");
        return false;
    }

    running_.store(true);

    // 수신 스레드 시작
    recv_thread_ = std::thread(&FIXFeedHandler::recv_loop, this);

    // Heartbeat 스레드 시작
    heartbeat_thread_ = std::thread(&FIXFeedHandler::heartbeat_loop, this);

    status_.store(FeedStatus::CONNECTED);
    return true;
}

void FIXFeedHandler::disconnect() {
    if (!running_.load()) return;

    running_.store(false);
    status_.store(FeedStatus::DISCONNECTED);

    close_socket();

    if (recv_thread_.joinable()) {
        recv_thread_.join();
    }

    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }
}

bool FIXFeedHandler::is_connected() const {
    return status_.load() == FeedStatus::CONNECTED ||
           status_.load() == FeedStatus::ACTIVE;
}

bool FIXFeedHandler::subscribe(const std::vector<std::string>& symbols) {
    if (!is_connected()) return false;

    {
        std::lock_guard<std::mutex> lock(symbols_mutex_);
        subscribed_symbols_.insert(subscribed_symbols_.end(),
                                    symbols.begin(), symbols.end());
    }

    status_.store(FeedStatus::SUBSCRIBING);
    bool success = send_market_data_request(symbols);
    if (success) {
        status_.store(FeedStatus::ACTIVE);
    }
    return success;
}

bool FIXFeedHandler::unsubscribe(const std::vector<std::string>& symbols) {
    std::lock_guard<std::mutex> lock(symbols_mutex_);
    for (const auto& sym : symbols)
        subscribed_symbols_.erase(sym);
    return true;
}

void FIXFeedHandler::on_tick(TickCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    tick_callback_ = callback;
}

void FIXFeedHandler::on_quote(QuoteCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    quote_callback_ = callback;
}

void FIXFeedHandler::on_order(OrderCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    order_callback_ = callback;
}

void FIXFeedHandler::on_error(ErrorCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    error_callback_ = callback;
}

FeedStatus FIXFeedHandler::get_status() const {
    return status_.load();
}

// ============================================================================
// Private Methods
// ============================================================================

bool FIXFeedHandler::connect_socket() {
    sockfd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd_ < 0) {
        return false;
    }

    struct sockaddr_in server_addr;
    std::memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(config_.port);

    if (inet_pton(AF_INET, config_.host.c_str(), &server_addr.sin_addr) <= 0) {
        close(sockfd_);
        sockfd_ = -1;
        return false;
    }

    if (::connect(sockfd_, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(sockfd_);
        sockfd_ = -1;
        return false;
    }

    return true;
}

void FIXFeedHandler::close_socket() {
    if (sockfd_ >= 0) {
        ::close(sockfd_);
        sockfd_ = -1;
    }
}

bool FIXFeedHandler::send_logon() {
    std::string logon_msg = builder_.build_logon(config_.heartbeat_interval_ms / 1000);
    ssize_t sent = ::send(sockfd_, logon_msg.c_str(), logon_msg.length(), 0);
    return sent == static_cast<ssize_t>(logon_msg.length());
}

bool FIXFeedHandler::send_heartbeat() {
    std::string hb_msg = builder_.build_heartbeat();
    ssize_t sent = ::send(sockfd_, hb_msg.c_str(), hb_msg.length(), 0);
    return sent == static_cast<ssize_t>(hb_msg.length());
}

bool FIXFeedHandler::send_market_data_request(const std::vector<std::string>& symbols) {
    std::string msg = builder_.build_market_data_request(symbols);
    ssize_t sent = ::send(sockfd_, msg.c_str(), msg.length(), 0);
    return sent == static_cast<ssize_t>(msg.length());
}

void FIXFeedHandler::recv_loop() {
    std::vector<char> buffer(config_.buffer_size);

    while (running_.load()) {
        ssize_t received = ::recv(sockfd_, buffer.data(), buffer.size(), 0);

        if (received <= 0) {
            if (running_.load()) {
                notify_error("Connection closed");
                running_.store(false);
            }
            break;
        }

        byte_count_.fetch_add(received);
        handle_message(buffer.data(), received);
    }
}

void FIXFeedHandler::heartbeat_loop() {
    auto interval = std::chrono::milliseconds(config_.heartbeat_interval_ms);

    while (running_.load()) {
        std::this_thread::sleep_for(interval);

        if (!running_.load()) break;

        if (!send_heartbeat()) {
            notify_error("Failed to send heartbeat");
            running_.store(false);
            break;
        }
    }
}

void FIXFeedHandler::handle_message(const char* msg, size_t len) {
    // FIX 메시지는 여러 개가 합쳐져 올 수 있음
    // 간단화: 한 번에 하나만 처리
    if (!parser_.parse(msg, len)) {
        error_count_.fetch_add(1);
        return;
    }

    message_count_.fetch_add(1);

    auto msg_type = parser_.get_msg_type();

    // Heartbeat 응답 (무시)
    if (msg_type == FIXMsgType::HEARTBEAT) {
        return;
    }

    // Tick 추출
    Tick tick;
    if (parser_.extract_tick(tick, mapper_)) {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (tick_callback_) {
            tick_callback_(tick);
        }
        return;
    }

    // Quote 추출
    Quote quote;
    if (parser_.extract_quote(quote, mapper_)) {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (quote_callback_) {
            quote_callback_(quote);
        }
        return;
    }

    // Order 추출
    Order order;
    if (parser_.extract_order(order, mapper_)) {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (order_callback_) {
            order_callback_(order);
        }
        return;
    }
}

void FIXFeedHandler::notify_error(const std::string& error) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    if (error_callback_) {
        error_callback_(error);
    }
}

} // namespace zeptodb::feeds
