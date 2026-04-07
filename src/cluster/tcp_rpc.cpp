// ============================================================================
// Phase C-3: TcpRpcServer + TcpRpcClient implementation
// ============================================================================

#include "zeptodb/cluster/tcp_rpc.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "zeptodb/common/logger.h"

namespace zeptodb::cluster {

// ============================================================================
// Low-level helpers
// ============================================================================

static bool send_all(int fd, const void* buf, size_t len) {
    const auto* p = static_cast<const uint8_t*>(buf);
    size_t rem = len;
    while (rem > 0) {
        ssize_t n = ::send(fd, p, rem, MSG_NOSIGNAL);
        if (n <= 0) return false;
        p   += n;
        rem -= static_cast<size_t>(n);
    }
    return true;
}

static bool recv_all(int fd, void* buf, size_t len) {
    auto* p = static_cast<uint8_t*>(buf);
    size_t rem = len;
    while (rem > 0) {
        ssize_t n = ::recv(fd, p, rem, 0);
        if (n <= 0) return false;
        p   += n;
        rem -= static_cast<size_t>(n);
    }
    return true;
}

static bool send_message(int fd, RpcType type, uint32_t req_id,
                         const std::vector<uint8_t>& payload) {
    RpcHeader hdr;
    hdr.type        = static_cast<uint32_t>(type);
    hdr.request_id  = req_id;
    hdr.payload_len = static_cast<uint32_t>(payload.size());

    if (!send_all(fd, &hdr, sizeof(hdr))) return false;
    if (!payload.empty() && !send_all(fd, payload.data(), payload.size())) return false;
    return true;
}

// ============================================================================
// TcpRpcServer
// ============================================================================

void TcpRpcServer::start(uint16_t port, SqlQueryCallback   sql_cb,
                          TickIngestCallback tick_cb,
                          WalReplayCallback  wal_cb) {
    if (running_.load()) return;

    sql_callback_  = std::move(sql_cb);
    tick_callback_ = std::move(tick_cb);
    wal_callback_  = std::move(wal_cb);
    port_          = port;

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return;

    int opt = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return;
    }
    if (::listen(listen_fd_, 64) < 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return;
    }

    running_.store(true);

    // Start worker thread pool
    size_t n = pool_size_ > 0 ? pool_size_ : std::thread::hardware_concurrency();
    if (n == 0) n = 4;
    workers_.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        workers_.emplace_back([this]() { worker_loop(); });
    }

    accept_thread_ = std::thread([this]() { accept_loop(); });
}

void TcpRpcServer::stop() {
    if (!running_.exchange(false)) return;

    // 1. Stop accepting new connections
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (accept_thread_.joinable()) accept_thread_.join();

    // 2. Drain pending queue (no worker will pick these up after running_=false)
    {
        std::lock_guard<std::mutex> lock(queue_mu_);
        while (!conn_queue_.empty()) {
            ::close(conn_queue_.front());
            conn_queue_.pop();
            active_conns_.fetch_sub(1);
        }
    }
    // Wake workers — they will finish current connection then exit
    queue_cv_.notify_all();

    // 3. Wait for in-flight requests to complete (graceful drain)
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(drain_timeout_ms_);
    while (active_conns_.load() > 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // 4. Force-close remaining connections if drain timed out
    bool force = active_conns_.load() > 0;
    if (force) {
        ZEPTO_WARN("TcpRpcServer: drain timeout ({}ms), force-closing {} connections",
                   drain_timeout_ms_, active_conns_.load());
        std::lock_guard<std::mutex> lock(conn_fds_mu_);
        for (int fd : conn_fds_) ::shutdown(fd, SHUT_RDWR);
    }

    // 5. Join all worker threads.
    //    After force-closing sockets above, blocked recv/send will return
    //    with an error, so workers will exit promptly.  Never detach —
    //    detached threads may access destroyed members after ~TcpRpcServer.
    for (auto& w : workers_) {
        if (w.joinable()) w.join();
    }
    workers_.clear();
}

void TcpRpcServer::accept_loop() {
    while (running_.load()) {
        struct sockaddr_in client_addr{};
        socklen_t clen = sizeof(client_addr);
        int cfd = ::accept(listen_fd_,
                           reinterpret_cast<sockaddr*>(&client_addr), &clen);
        if (cfd < 0) {
            if (!running_.load()) break;
            continue;
        }

        active_conns_.fetch_add(1);
        if (active_conns_.load() > max_connections_) {
            ZEPTO_WARN("TcpRpcServer: max connections reached ({}/{}), rejecting",
                       active_conns_.load(), max_connections_);
            ::close(cfd);
            active_conns_.fetch_sub(1);
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(queue_mu_);
            conn_queue_.push(cfd);
        }
        queue_cv_.notify_one();
    }
}

void TcpRpcServer::worker_loop() {
    while (true) {
        int cfd;
        {
            std::unique_lock<std::mutex> lock(queue_mu_);
            queue_cv_.wait(lock, [this]() {
                return !conn_queue_.empty() || !running_.load();
            });
            if (!running_.load() && conn_queue_.empty()) return;
            cfd = conn_queue_.front();
            conn_queue_.pop();
        }
        {
            std::lock_guard<std::mutex> lock(conn_fds_mu_);
            conn_fds_.push_back(cfd);
        }
        handle_connection(cfd);
        {
            std::lock_guard<std::mutex> lock(conn_fds_mu_);
            conn_fds_.erase(std::remove(conn_fds_.begin(), conn_fds_.end(), cfd),
                            conn_fds_.end());
        }
        ::close(cfd);
        active_conns_.fetch_sub(1);
    }
}

void TcpRpcServer::handle_connection(int cfd) {
    // Set recv timeout so keep-alive connections don't block forever
    struct timeval tv;
    tv.tv_sec  = 1;
    tv.tv_usec = 0;
    ::setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Auth handshake: if security enabled, first message must be AUTH_HANDSHAKE
    if (security_.enabled) {
        RpcHeader auth_hdr;
        if (!recv_all(cfd, &auth_hdr, sizeof(auth_hdr))) return;
        if (auth_hdr.magic != 0x41504558u ||
            static_cast<RpcType>(auth_hdr.type) != RpcType::AUTH_HANDSHAKE ||
            auth_hdr.payload_len < RPC_AUTH_PAYLOAD_SIZE ||
            auth_hdr.payload_len > max_payload_size_) {
            // Not an auth message — reject
            RpcHeader rej{};
            rej.type = static_cast<uint32_t>(RpcType::AUTH_REJECT);
            send_all(cfd, &rej, sizeof(rej));
            return;
        }
        std::vector<uint8_t> auth_payload(auth_hdr.payload_len);
        if (!recv_all(cfd, auth_payload.data(), auth_hdr.payload_len)) return;

        if (!rpc_validate_auth(security_.shared_secret,
                               auth_payload.data(), auth_payload.size())) {
            ZEPTO_WARN("TcpRpcServer: auth handshake failed from client");
            RpcHeader rej{};
            rej.type = static_cast<uint32_t>(RpcType::AUTH_REJECT);
            send_all(cfd, &rej, sizeof(rej));
            return;
        }

        // Auth OK
        RpcHeader ok_hdr{};
        ok_hdr.type = static_cast<uint32_t>(RpcType::AUTH_OK);
        if (!send_all(cfd, &ok_hdr, sizeof(ok_hdr))) return;
    }

    RpcHeader hdr;
    while (running_.load()) {
        if (!recv_all(cfd, &hdr, sizeof(hdr))) break;
        if (hdr.magic != 0x41504558u) break;

        // Reject oversized payloads to prevent OOM
        if (hdr.payload_len > max_payload_size_) {
            ZEPTO_WARN("TcpRpcServer: payload too large ({} > {} bytes), closing connection",
                       hdr.payload_len, max_payload_size_);
            break;
        }

        std::vector<uint8_t> payload(hdr.payload_len);
        if (hdr.payload_len > 0 && !recv_all(cfd, payload.data(), hdr.payload_len)) break;

        const RpcType type = static_cast<RpcType>(hdr.type);

        if (type == RpcType::PING) {
            RpcHeader pong;
            pong.type        = static_cast<uint32_t>(RpcType::PONG);
            pong.request_id  = hdr.request_id;
            pong.payload_len = 0;
            send_all(cfd, &pong, sizeof(pong));
            continue;
        }

        if (type == RpcType::SQL_QUERY) {
            std::string sql(reinterpret_cast<const char*>(payload.data()), payload.size());
            zeptodb::sql::QueryResultSet result;
            try {
                result = sql_callback_(sql);
            } catch (const std::exception& e) {
                result.error = std::string("rpc_server: ") + e.what();
            } catch (...) {
                result.error = "rpc_server: unknown exception";
            }
            auto data = serialize_result(result);
            if (!send_message(cfd, RpcType::SQL_RESULT, hdr.request_id, data))
                break;
            continue;  // keep-alive: wait for next request
        }

        if (type == RpcType::TICK_INGEST) {
            // Fencing: reject stale-epoch writes
            if (fencing_token_ && hdr.epoch != 0 &&
                !fencing_token_->validate(hdr.epoch)) {
                RpcHeader ack;
                ack.type        = static_cast<uint32_t>(RpcType::TICK_ACK);
                ack.request_id  = hdr.request_id;
                ack.payload_len = 1;
                uint8_t status  = 0u;  // rejected
                if (!send_all(cfd, &ack, sizeof(ack)) || !send_all(cfd, &status, 1))
                    break;
                continue;
            }
            bool ok = false;
            if (tick_callback_) {
                zeptodb::ingestion::TickMessage tick_msg{};
                if (deserialize_tick(payload.data(), payload.size(), tick_msg)) {
                    try {
                        ok = tick_callback_(tick_msg);
                    } catch (...) {
                        ok = false;
                    }
                }
            }
            RpcHeader ack;
            ack.type        = static_cast<uint32_t>(RpcType::TICK_ACK);
            ack.request_id  = hdr.request_id;
            ack.payload_len = 1;
            uint8_t status  = ok ? 1u : 0u;
            if (!send_all(cfd, &ack, sizeof(ack)) || !send_all(cfd, &status, 1))
                break;
            continue;  // keep-alive
        }

        if (type == RpcType::WAL_REPLICATE) {
            // Fencing: reject stale-epoch writes
            if (fencing_token_ && hdr.epoch != 0 &&
                !fencing_token_->validate(hdr.epoch)) {
                RpcHeader ack;
                ack.type        = static_cast<uint32_t>(RpcType::WAL_ACK);
                ack.request_id  = hdr.request_id;
                ack.payload_len = 1;
                uint8_t status  = 0u;  // rejected
                if (!send_all(cfd, &ack, sizeof(ack)) || !send_all(cfd, &status, 1))
                    break;
                continue;
            }
            size_t applied = 0;
            if (wal_callback_) {
                std::vector<zeptodb::ingestion::TickMessage> batch;
                if (deserialize_wal_batch(payload.data(), payload.size(), batch)) {
                    try { applied = wal_callback_(batch); } catch (...) {}
                }
            }
            RpcHeader ack;
            ack.type        = static_cast<uint32_t>(RpcType::WAL_ACK);
            ack.request_id  = hdr.request_id;
            ack.payload_len = 1;
            uint8_t status  = (applied > 0) ? 1u : 0u;
            if (!send_all(cfd, &ack, sizeof(ack)) || !send_all(cfd, &status, 1))
                break;
            continue;
        }

        if (type == RpcType::STATS_REQUEST) {
            std::string json = "{}";
            if (stats_callback_) {
                try { json = stats_callback_(); } catch (...) {}
            }
            RpcHeader resp;
            resp.type        = static_cast<uint32_t>(RpcType::STATS_RESULT);
            resp.request_id  = hdr.request_id;
            resp.payload_len = static_cast<uint32_t>(json.size());
            if (!send_all(cfd, &resp, sizeof(resp)) ||
                !send_all(cfd, json.data(), json.size()))
                break;
            continue;
        }

        if (type == RpcType::METRICS_REQUEST) {
            std::string json = "[]";
            if (metrics_callback_ && payload.size() >= 12) {
                int64_t since_ms = 0;
                uint32_t limit = 0;
                deserialize_metrics_request(payload.data(), payload.size(),
                                            since_ms, limit);
                try { json = metrics_callback_(since_ms, limit); } catch (...) {}
            }
            RpcHeader resp;
            resp.type        = static_cast<uint32_t>(RpcType::METRICS_RESULT);
            resp.request_id  = hdr.request_id;
            resp.payload_len = static_cast<uint32_t>(json.size());
            if (!send_all(cfd, &resp, sizeof(resp)) ||
                !send_all(cfd, json.data(), json.size()))
                break;
            continue;
        }

        if (type == RpcType::RING_UPDATE) {
            bool applied = false;
            if (ring_update_callback_) {
                try {
                    applied = ring_update_callback_(payload.data(), payload.size());
                } catch (...) {}
            }
            RpcHeader ack{};
            ack.type        = static_cast<uint32_t>(RpcType::RING_ACK);
            ack.request_id  = hdr.request_id;
            ack.payload_len = 0;
            if (!send_all(cfd, &ack, sizeof(ack)))
                break;
            continue;
        }

        break;  // Unknown type
    }
}

// ============================================================================
// TcpRpcClient — connection pool
// ============================================================================

TcpRpcClient::~TcpRpcClient() {
    std::lock_guard<std::mutex> lock(pool_mu_);
    for (int fd : pool_) ::close(fd);
    pool_.clear();
}

size_t TcpRpcClient::pool_idle_count() const {
    std::lock_guard<std::mutex> lock(pool_mu_);
    return pool_.size();
}

int TcpRpcClient::acquire() {
    {
        std::lock_guard<std::mutex> lock(pool_mu_);
        while (!pool_.empty()) {
            int fd = pool_.back();
            pool_.pop_back();
            // Quick liveness check: non-blocking recv should return EAGAIN on a healthy idle socket
            char probe;
            ssize_t n = ::recv(fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                ::close(fd);  // stale
                continue;
            }
            return fd;
        }
    }
    int fd = connect_to_server();
    if (fd < 0) return -1;

    // Auth handshake on new connections
    if (security_.enabled && !security_.shared_secret.empty()) {
        auto auth_payload = rpc_build_auth(security_.shared_secret);
        RpcHeader auth_hdr{};
        auth_hdr.type        = static_cast<uint32_t>(RpcType::AUTH_HANDSHAKE);
        auth_hdr.payload_len = static_cast<uint32_t>(auth_payload.size());
        if (!send_all(fd, &auth_hdr, sizeof(auth_hdr)) ||
            !send_all(fd, auth_payload.data(), auth_payload.size())) {
            ::close(fd);
            return -1;
        }
        // Wait for AUTH_OK
        RpcHeader resp{};
        if (!recv_all(fd, &resp, sizeof(resp)) ||
            static_cast<RpcType>(resp.type) != RpcType::AUTH_OK) {
            ::close(fd);
            return -1;
        }
    }

    return fd;
}

void TcpRpcClient::release(int fd, bool healthy) {
    if (fd < 0) return;
    if (!healthy) { ::close(fd); return; }
    std::lock_guard<std::mutex> lock(pool_mu_);
    if (pool_.size() < max_pool_size_) {
        pool_.push_back(fd);
    } else {
        ::close(fd);
    }
}

int TcpRpcClient::connect_to_server() const {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port_);

    // Try inet_pton first (numeric IP), fall back to getaddrinfo (hostname)
    if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) <= 0) {
        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (::getaddrinfo(host_.c_str(), nullptr, &hints, &res) != 0 || !res) {
            ::close(fd);
            return -1;
        }
        addr.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
        ::freeaddrinfo(res);
    }

    if (connect_timeout_ms_ <= 0) {
        // Blocking connect (no timeout)
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(fd);
            return -1;
        }
        return fd;
    }

    // Non-blocking connect + select() for configurable timeout
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        ::close(fd);
        return -1;
    }

    int ret = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        ::close(fd);
        return -1;
    }

    if (ret != 0) {
        // Wait for writability with timeout
        fd_set wfds, efds;
        FD_ZERO(&wfds); FD_SET(fd, &wfds);
        FD_ZERO(&efds); FD_SET(fd, &efds);
        struct timeval tv;
        tv.tv_sec  = connect_timeout_ms_ / 1000;
        tv.tv_usec = (connect_timeout_ms_ % 1000) * 1000;

        int sel = ::select(fd + 1, nullptr, &wfds, &efds, &tv);
        if (sel <= 0) {
            // Timeout (sel==0) or select error (sel<0)
            ::close(fd);
            return -1;
        }

        // Verify the connection actually succeeded
        int err = 0;
        socklen_t elen = sizeof(err);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err != 0) {
            ::close(fd);
            return -1;
        }
    }

    // Restore blocking mode for subsequent send/recv
    ::fcntl(fd, F_SETFL, flags);
    return fd;
}

zeptodb::sql::QueryResultSet TcpRpcClient::execute_sql(const std::string& sql) {
    zeptodb::sql::QueryResultSet err_result;

    int fd = acquire();
    if (fd < 0) {
        err_result.error = "TcpRpcClient: cannot connect to "
                         + host_ + ":" + std::to_string(port_);
        return err_result;
    }

    // P0-3: Set per-query read timeout on the socket
    if (query_timeout_ms_ > 0) {
        struct timeval tv;
        tv.tv_sec  = query_timeout_ms_ / 1000;
        tv.tv_usec = (query_timeout_ms_ % 1000) * 1000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    std::vector<uint8_t> payload(sql.begin(), sql.end());
    if (!send_message(fd, RpcType::SQL_QUERY, 1, payload)) {
        release(fd, false);
        err_result.error = "TcpRpcClient: send failed";
        return err_result;
    }

    RpcHeader hdr{};
    if (!recv_all(fd, &hdr, sizeof(hdr))) {
        release(fd, false);
        err_result.error = (errno == EAGAIN || errno == EWOULDBLOCK)
            ? "TcpRpcClient: query timeout (" + std::to_string(query_timeout_ms_) + "ms)"
            : "TcpRpcClient: recv header failed";
        return err_result;
    }

    std::vector<uint8_t> resp(hdr.payload_len);
    if (hdr.payload_len > 0 && !recv_all(fd, resp.data(), hdr.payload_len)) {
        release(fd, false);
        err_result.error = "TcpRpcClient: recv payload failed";
        return err_result;
    }

    release(fd, true);  // return to pool

    if (static_cast<RpcType>(hdr.type) != RpcType::SQL_RESULT) {
        err_result.error = "TcpRpcClient: unexpected response type";
        return err_result;
    }

    return deserialize_result(resp.data(), resp.size());
}

bool TcpRpcClient::ingest_tick(const zeptodb::ingestion::TickMessage& msg) {
    int fd = acquire();
    if (fd < 0) return false;

    auto payload = serialize_tick(msg);
    RpcHeader hdr;
    hdr.type        = static_cast<uint32_t>(RpcType::TICK_INGEST);
    hdr.request_id  = 1;
    hdr.payload_len = static_cast<uint32_t>(payload.size());
    hdr.epoch       = epoch_;
    if (!send_all(fd, &hdr, sizeof(hdr)) ||
        (!payload.empty() && !send_all(fd, payload.data(), payload.size()))) {
        release(fd, false);
        return false;
    }

    RpcHeader resp{};
    if (!recv_all(fd, &resp, sizeof(resp))) {
        release(fd, false);
        return false;
    }

    uint8_t status = 0;
    if (resp.payload_len == 1) {
        if (!recv_all(fd, &status, 1)) {
            release(fd, false);
            return false;
        }
    }
    release(fd, true);
    return static_cast<RpcType>(resp.type) == RpcType::TICK_ACK && status == 1u;
}

bool TcpRpcClient::replicate_wal(
    const std::vector<zeptodb::ingestion::TickMessage>& batch)
{
    if (batch.empty()) return true;
    int fd = acquire();
    if (fd < 0) return false;

    auto payload = serialize_wal_batch(batch);
    RpcHeader hdr;
    hdr.type        = static_cast<uint32_t>(RpcType::WAL_REPLICATE);
    hdr.request_id  = 1;
    hdr.payload_len = static_cast<uint32_t>(payload.size());
    hdr.epoch       = epoch_;
    if (!send_all(fd, &hdr, sizeof(hdr)) ||
        (!payload.empty() && !send_all(fd, payload.data(), payload.size()))) {
        release(fd, false);
        return false;
    }

    RpcHeader resp{};
    if (!recv_all(fd, &resp, sizeof(resp))) {
        release(fd, false);
        return false;
    }
    uint8_t status = 0;
    if (resp.payload_len == 1) {
        if (!recv_all(fd, &status, 1)) {
            release(fd, false);
            return false;
        }
    }
    release(fd, true);
    return static_cast<RpcType>(resp.type) == RpcType::WAL_ACK && status == 1u;
}

std::string TcpRpcClient::request_stats() {
    int fd = acquire();
    if (fd < 0) return "";

    RpcHeader hdr{};
    hdr.type        = static_cast<uint32_t>(RpcType::STATS_REQUEST);
    hdr.request_id  = 1;
    hdr.payload_len = 0;
    if (!send_all(fd, &hdr, sizeof(hdr))) {
        release(fd, false);
        return "";
    }

    RpcHeader resp{};
    if (!recv_all(fd, &resp, sizeof(resp)) ||
        static_cast<RpcType>(resp.type) != RpcType::STATS_RESULT) {
        release(fd, false);
        return "";
    }

    std::string json(resp.payload_len, '\0');
    if (resp.payload_len > 0 && !recv_all(fd, json.data(), resp.payload_len)) {
        release(fd, false);
        return "";
    }
    release(fd, true);
    return json;
}

std::string TcpRpcClient::request_metrics(int64_t since_ms, uint32_t limit) {
    int fd = acquire();
    if (fd < 0) return "";

    auto payload = serialize_metrics_request(since_ms, limit);

    RpcHeader hdr{};
    hdr.type        = static_cast<uint32_t>(RpcType::METRICS_REQUEST);
    hdr.request_id  = 1;
    hdr.payload_len = static_cast<uint32_t>(payload.size());
    if (!send_all(fd, &hdr, sizeof(hdr)) ||
        !send_all(fd, payload.data(), payload.size())) {
        release(fd, false);
        return "";
    }

    RpcHeader resp{};
    if (!recv_all(fd, &resp, sizeof(resp)) ||
        static_cast<RpcType>(resp.type) != RpcType::METRICS_RESULT) {
        release(fd, false);
        return "";
    }

    std::string json(resp.payload_len, '\0');
    if (resp.payload_len > 0 && !recv_all(fd, json.data(), resp.payload_len)) {
        release(fd, false);
        return "";
    }
    release(fd, true);
    return json;
}

bool TcpRpcClient::ping() {
    int fd = acquire();
    if (fd < 0) return false;

    RpcHeader hdr;
    hdr.type        = static_cast<uint32_t>(RpcType::PING);
    hdr.request_id  = 0;
    hdr.payload_len = 0;

    if (!send_all(fd, &hdr, sizeof(hdr))) {
        release(fd, false);
        return false;
    }

    RpcHeader resp{};
    bool ok = recv_all(fd, &resp, sizeof(resp));
    release(fd, ok);
    return ok && static_cast<RpcType>(resp.type) == RpcType::PONG;
}

} // namespace zeptodb::cluster
