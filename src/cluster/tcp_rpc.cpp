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
#include <limits>
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
                         const std::vector<uint8_t>& payload,
                         size_t max_payload_size =
                             std::numeric_limits<uint32_t>::max()) {
    if (payload.size() > max_payload_size ||
        payload.size() > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    RpcHeader hdr;
    hdr.type        = static_cast<uint32_t>(type);
    hdr.request_id  = req_id;
    hdr.payload_len = static_cast<uint32_t>(payload.size());

    if (!send_all(fd, &hdr, sizeof(hdr))) return false;
    if (!payload.empty() && !send_all(fd, payload.data(), payload.size())) return false;
    return true;
}

static bool serialized_result_exceeds(
    const zeptodb::sql::QueryResultSet& result,
    size_t limit) {
    size_t total = 0;
    auto add = [&](size_t bytes) {
        if (bytes > limit - std::min(total, limit)) return false;
        total += bytes;
        return total <= limit;
    };

    if (!add(sizeof(uint32_t)) || !add(result.error.size()) ||
        result.column_names.size() > std::numeric_limits<uint32_t>::max() ||
        !add(sizeof(uint32_t))) {
        return true;
    }
    for (const auto& name : result.column_names) {
        if (name.size() > std::numeric_limits<uint32_t>::max() ||
            !add(sizeof(uint32_t)) || !add(name.size()) ||
            !add(sizeof(uint8_t))) {
            return true;
        }
    }
    if (result.rows.size() > std::numeric_limits<uint32_t>::max() ||
        !add(sizeof(uint32_t))) {
        return true;
    }
    const size_t column_count = result.column_names.size();
    if (column_count != 0 &&
        result.rows.size() >
            std::numeric_limits<size_t>::max() / column_count) {
        return true;
    }
    const size_t cell_count = result.rows.size() * column_count;
    if (cell_count > std::numeric_limits<size_t>::max() / sizeof(int64_t) ||
        !add(cell_count * sizeof(int64_t)) || !add(sizeof(uint32_t))) {
        return true;
    }

    size_t string_count = 0;
    if (!result.string_rows.empty()) {
        string_count = result.string_rows.size();
        if (string_count > std::numeric_limits<uint32_t>::max()) return true;
        for (const auto& value : result.string_rows) {
            if (value.size() > std::numeric_limits<uint32_t>::max() ||
                !add(sizeof(uint32_t)) || !add(value.size())) {
                return true;
            }
        }
    } else if (result.symbol_dict != nullptr) {
        for (const auto& row : result.rows) {
            for (size_t column = 0;
                 column < column_count && column < row.size(); ++column) {
                if (column >= result.column_types.size() ||
                    !rpc_is_string_encoded_type(
                        result.column_types[column])) {
                    continue;
                }
                if (++string_count > std::numeric_limits<uint32_t>::max()) {
                    return true;
                }
                const auto value = result.symbol_dict->lookup(
                    static_cast<uint32_t>(row[column]));
                if (value.size() > std::numeric_limits<uint32_t>::max() ||
                    !add(sizeof(uint32_t)) || !add(value.size())) {
                    return true;
                }
            }
        }
    }
    return total > limit;
}

static bool send_auth_header(int fd, RpcType type, uint32_t payload_len = 0) {
    RpcHeader header{};
    header.type = static_cast<uint32_t>(type);
    header.payload_len = payload_len;
    return send_all(fd, &header, sizeof(header));
}

static void reject_auth(int fd) {
    (void)send_auth_header(fd, RpcType::AUTH_REJECT);
}

// ============================================================================
// TcpRpcServer
// ============================================================================

void TcpRpcServer::start(uint16_t port, SqlQueryCallback   sql_cb,
                          TickIngestCallback tick_cb,
                          WalReplayCallback  wal_cb) {
    if (running_.load()) return;

    const std::string security_error = security_.validation_error();
    if (!security_error.empty()) {
        ZEPTO_ERROR("TcpRpcServer: invalid cluster RPC security configuration: {}",
                    security_error);
        return;
    }

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

    // A peer that authenticates and then stops reading must not pin a worker
    // forever in send_all(). Use the graceful-drain bound for server writes.
    const int send_timeout_ms = std::max(drain_timeout_ms_, 1);
    struct timeval send_timeout{};
    send_timeout.tv_sec = send_timeout_ms / 1000;
    send_timeout.tv_usec = (send_timeout_ms % 1000) * 1000;
    ::setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO,
                 &send_timeout, sizeof(send_timeout));

    // Protocol v2 mutually authenticates both peers and binds each proof to a
    // fresh server challenge. Authentication is performed once per pooled
    // connection before any application message is accepted.
    if (security_.enabled) {
        RpcHeader hello_header{};
        if (!recv_all(cfd, &hello_header, sizeof(hello_header))) return;
        if (hello_header.magic != 0x41504558u ||
            static_cast<RpcType>(hello_header.type) !=
                RpcType::AUTH_CLIENT_HELLO ||
            hello_header.payload_len != RPC_AUTH_CLIENT_HELLO_SIZE) {
            reject_auth(cfd);
            return;
        }

        RpcAuthNonce client_nonce{};
        if (!recv_all(cfd, client_nonce.data(), client_nonce.size())) return;

        RpcAuthNonce server_nonce{};
        RpcAuthProof server_proof{};
        if (!rpc_generate_auth_nonce(server_nonce) ||
            !rpc_compute_server_proof(security_.shared_secret, client_nonce,
                                      server_nonce, server_proof)) {
            ZEPTO_ERROR("TcpRpcServer: could not create RPC auth challenge");
            reject_auth(cfd);
            return;
        }

        RpcAuthServerChallenge challenge{};
        std::copy(server_nonce.begin(), server_nonce.end(), challenge.begin());
        std::copy(server_proof.begin(), server_proof.end(),
                  challenge.begin() + RPC_AUTH_NONCE_SIZE);
        if (!send_auth_header(
                cfd, RpcType::AUTH_SERVER_CHALLENGE,
                static_cast<uint32_t>(challenge.size())) ||
            !send_all(cfd, challenge.data(), challenge.size())) {
            return;
        }

        RpcHeader proof_header{};
        if (!recv_all(cfd, &proof_header, sizeof(proof_header))) return;
        if (proof_header.magic != 0x41504558u ||
            static_cast<RpcType>(proof_header.type) !=
                RpcType::AUTH_CLIENT_PROOF ||
            proof_header.payload_len != RPC_AUTH_CLIENT_PROOF_SIZE) {
            reject_auth(cfd);
            return;
        }

        RpcAuthProof client_proof{};
        if (!recv_all(cfd, client_proof.data(), client_proof.size())) return;
        if (!rpc_validate_client_proof(security_.shared_secret, client_nonce,
                                       server_nonce, client_proof)) {
            ZEPTO_WARN("TcpRpcServer: cluster RPC authentication failed");
            reject_auth(cfd);
            return;
        }

        if (!send_auth_header(cfd, RpcType::AUTH_OK)) return;
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
            if (serialized_result_exceeds(result, max_payload_size_)) {
                result = {};
                result.error =
                    "rpc_server: response exceeds configured payload limit";
            }
            auto data = serialize_result(result);
            if (!send_message(cfd, RpcType::SQL_RESULT, hdr.request_id, data,
                              max_payload_size_))
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

        if (type == RpcType::TYPED_ROW_INGEST) {
            // Fencing: reject stale-epoch writes
            if (fencing_token_ && hdr.epoch != 0 &&
                !fencing_token_->validate(hdr.epoch)) {
                RpcHeader ack;
                ack.type        = static_cast<uint32_t>(RpcType::TYPED_ROW_ACK);
                ack.request_id  = hdr.request_id;
                ack.payload_len = 1;
                uint8_t status  = 0u;  // rejected
                if (!send_all(cfd, &ack, sizeof(ack)) || !send_all(cfd, &status, 1))
                    break;
                continue;
            }
            bool ok = false;
            if (typed_row_callback_) {
                zeptodb::core::TypedRowMessage row{};
                if (deserialize_typed_row(payload.data(), payload.size(), row)) {
                    try {
                        ok = typed_row_callback_(std::move(row));
                    } catch (...) {
                        ok = false;
                    }
                }
            }
            RpcHeader ack;
            ack.type        = static_cast<uint32_t>(RpcType::TYPED_ROW_ACK);
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

        if (type == RpcType::AGENT_MEMORY_PUT ||
            type == RpcType::AGENT_CACHE_STORE ||
            type == RpcType::AGENT_MEMORY_DELETE ||
            type == RpcType::AGENT_CACHE_DELETE) {
            const RpcType response_type =
                (type == RpcType::AGENT_MEMORY_PUT ||
                 type == RpcType::AGENT_MEMORY_DELETE)
                ? RpcType::AGENT_MEMORY_RESULT
                : RpcType::AGENT_CACHE_RESULT;
            if (fencing_token_ && hdr.epoch != 0 &&
                !fencing_token_->validate(hdr.epoch)) {
                if (!send_message(cfd, response_type, hdr.request_id, {},
                                  max_payload_size_))
                    break;
                continue;
            }

            std::vector<uint8_t> response;
            BinaryRpcCallback* cb = nullptr;
            if (type == RpcType::AGENT_MEMORY_PUT) {
                cb = &agent_memory_put_callback_;
            } else if (type == RpcType::AGENT_CACHE_STORE) {
                cb = &agent_cache_store_callback_;
            } else if (type == RpcType::AGENT_MEMORY_DELETE) {
                cb = &agent_memory_delete_callback_;
            } else {
                cb = &agent_cache_delete_callback_;
            }
            if (*cb) {
                try {
                    response = (*cb)(payload.data(), payload.size());
                } catch (...) {
                    response.clear();
                }
            }
            if (!send_message(cfd, response_type, hdr.request_id, response,
                              max_payload_size_))
                break;
            continue;
        }

        if (type == RpcType::AGENT_MEMORY_REPLICA_APPEND) {
            if (fencing_token_ && hdr.epoch != 0 &&
                !fencing_token_->validate(hdr.epoch)) {
                const std::vector<uint8_t> rejected{0u};
                if (!send_message(cfd, RpcType::AGENT_MEMORY_REPLICA_ACK,
                                  hdr.request_id, rejected,
                                  max_payload_size_))
                    break;
                continue;
            }

            std::vector<uint8_t> response{0u};
            if (agent_memory_replica_append_callback_) {
                try {
                    response = agent_memory_replica_append_callback_(
                        payload.data(), payload.size());
                } catch (...) {
                    response = {0u};
                }
            }
            if (!send_message(cfd, RpcType::AGENT_MEMORY_REPLICA_ACK,
                              hdr.request_id, response, max_payload_size_))
                break;
            continue;
        }

        if (type == RpcType::AGENT_MEMORY_STATS) {
            std::vector<uint8_t> response;
            if (agent_memory_stats_callback_) {
                try {
                    response = agent_memory_stats_callback_(
                        payload.data(), payload.size());
                } catch (...) {
                    response.clear();
                }
            }
            if (!send_message(cfd, RpcType::AGENT_MEMORY_STATS_RESULT,
                              hdr.request_id, response, max_payload_size_))
                break;
            continue;
        }

        if (type == RpcType::AGENT_MEMORY_GET ||
            type == RpcType::AGENT_MEMORY_SEARCH ||
            type == RpcType::AGENT_CACHE_LOOKUP_EXACT) {
            RpcType response_type = RpcType::AGENT_CACHE_LOOKUP_RESULT;
            if (type == RpcType::AGENT_MEMORY_GET) {
                response_type = RpcType::AGENT_MEMORY_GET_RESULT;
            } else if (type == RpcType::AGENT_MEMORY_SEARCH) {
                response_type = RpcType::AGENT_MEMORY_SEARCH_RESULT;
            }
            std::vector<uint8_t> response;
            BinaryRpcCallback* cb = &agent_cache_lookup_callback_;
            if (type == RpcType::AGENT_MEMORY_GET) {
                cb = &agent_memory_get_callback_;
            } else if (type == RpcType::AGENT_MEMORY_SEARCH) {
                cb = &agent_memory_search_callback_;
            }
            if (*cb) {
                try {
                    response = (*cb)(payload.data(), payload.size());
                } catch (...) {
                    response.clear();
                }
            }
            if (!send_message(cfd, response_type, hdr.request_id, response,
                              max_payload_size_))
                break;
            continue;
        }

        if (type == RpcType::STATS_REQUEST) {
            std::string json = "{}";
            if (stats_callback_) {
                try { json = stats_callback_(); } catch (...) {}
            }
            if (json.size() > max_payload_size_) break;
            std::vector<uint8_t> response(json.begin(), json.end());
            if (!send_message(cfd, RpcType::STATS_RESULT, hdr.request_id,
                              response, max_payload_size_))
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
            if (json.size() > max_payload_size_) break;
            std::vector<uint8_t> response(json.begin(), json.end());
            if (!send_message(cfd, RpcType::METRICS_RESULT, hdr.request_id,
                              response, max_payload_size_))
                break;
            continue;
        }

        if (type == RpcType::RING_UPDATE) {
            if (ring_update_callback_) {
                try {
                    (void)ring_update_callback_(payload.data(), payload.size());
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

void TcpRpcClient::set_security(RpcSecurityConfig cfg) {
    std::lock_guard<std::mutex> lock(pool_mu_);
    for (int fd : pool_) ::close(fd);
    pool_.clear();
    security_ = std::move(cfg);
}

size_t TcpRpcClient::pool_idle_count() const {
    std::lock_guard<std::mutex> lock(pool_mu_);
    return pool_.size();
}

int TcpRpcClient::acquire() {
    RpcSecurityConfig security;
    {
        std::lock_guard<std::mutex> lock(pool_mu_);
        security = security_;
        if (!security.validation_error().empty()) return -1;
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

    // Auth handshake on new connections. Bound the handshake reads so a peer
    // cannot hold a client indefinitely before normal per-query timeouts apply.
    if (security.enabled) {
        const int auth_timeout_ms = connect_timeout_ms_ > 0
            ? connect_timeout_ms_
            : query_timeout_ms_;
        if (auth_timeout_ms > 0) {
            struct timeval timeout{};
            timeout.tv_sec = auth_timeout_ms / 1000;
            timeout.tv_usec = (auth_timeout_ms % 1000) * 1000;
            (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                               &timeout, sizeof(timeout));
            (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                               &timeout, sizeof(timeout));
        }

        RpcAuthNonce client_nonce{};
        if (!rpc_generate_auth_nonce(client_nonce) ||
            !send_auth_header(
                fd, RpcType::AUTH_CLIENT_HELLO,
                static_cast<uint32_t>(client_nonce.size())) ||
            !send_all(fd, client_nonce.data(), client_nonce.size())) {
            ::close(fd);
            return -1;
        }

        RpcHeader challenge_header{};
        if (!recv_all(fd, &challenge_header, sizeof(challenge_header)) ||
            challenge_header.magic != 0x41504558u ||
            static_cast<RpcType>(challenge_header.type) !=
                RpcType::AUTH_SERVER_CHALLENGE ||
            challenge_header.payload_len != RPC_AUTH_SERVER_CHALLENGE_SIZE) {
            ::close(fd);
            return -1;
        }

        RpcAuthServerChallenge challenge{};
        if (!recv_all(fd, challenge.data(), challenge.size())) {
            ::close(fd);
            return -1;
        }
        RpcAuthNonce server_nonce{};
        RpcAuthProof server_proof{};
        std::copy_n(challenge.begin(), server_nonce.size(),
                    server_nonce.begin());
        std::copy_n(challenge.begin() + RPC_AUTH_NONCE_SIZE,
                    server_proof.size(), server_proof.begin());
        if (!rpc_validate_server_proof(security.shared_secret, client_nonce,
                                       server_nonce, server_proof)) {
            ::close(fd);
            return -1;
        }

        RpcAuthProof client_proof{};
        if (!rpc_compute_client_proof(security.shared_secret, client_nonce,
                                      server_nonce, client_proof) ||
            !send_auth_header(
                fd, RpcType::AUTH_CLIENT_PROOF,
                static_cast<uint32_t>(client_proof.size())) ||
            !send_all(fd, client_proof.data(), client_proof.size())) {
            ::close(fd);
            return -1;
        }

        RpcHeader response_header{};
        if (!recv_all(fd, &response_header, sizeof(response_header)) ||
            response_header.magic != 0x41504558u ||
            static_cast<RpcType>(response_header.type) != RpcType::AUTH_OK ||
            response_header.payload_len != 0) {
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
    return execute_sql(sql, max_response_size_);
}

zeptodb::sql::QueryResultSet TcpRpcClient::execute_sql(
    const std::string& sql,
    size_t max_response_bytes) {
    zeptodb::sql::QueryResultSet err_result;
    const size_t response_limit = std::min<size_t>(
        max_response_bytes, max_response_size_);

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

    if (hdr.magic != 0x41504558u ||
        static_cast<RpcType>(hdr.type) != RpcType::SQL_RESULT) {
        release(fd, false);
        err_result.error = "TcpRpcClient: unexpected response type";
        return err_result;
    }

    if (static_cast<size_t>(hdr.payload_len) > response_limit) {
        release(fd, false);
        err_result.error =
            "TcpRpcClient: response payload exceeds configured limit (" +
            std::to_string(hdr.payload_len) + " > " +
            std::to_string(response_limit) + " bytes)";
        return err_result;
    }

    std::vector<uint8_t> resp(hdr.payload_len);
    if (hdr.payload_len > 0 && !recv_all(fd, resp.data(), hdr.payload_len)) {
        release(fd, false);
        err_result.error = "TcpRpcClient: recv payload failed";
        return err_result;
    }

    release(fd, true);  // return to pool

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

    if (resp.magic != 0x41504558u ||
        static_cast<RpcType>(resp.type) != RpcType::TICK_ACK ||
        resp.payload_len != 1) {
        release(fd, false);
        return false;
    }
    uint8_t status = 0;
    if (!recv_all(fd, &status, 1)) {
        release(fd, false);
        return false;
    }
    release(fd, true);
    return status == 1u;
}

bool TcpRpcClient::ingest_typed_row(const zeptodb::core::TypedRowMessage& row) {
    int fd = acquire();
    if (fd < 0) return false;

    auto payload = serialize_typed_row(row);
    RpcHeader hdr;
    hdr.type        = static_cast<uint32_t>(RpcType::TYPED_ROW_INGEST);
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

    if (resp.magic != 0x41504558u ||
        static_cast<RpcType>(resp.type) != RpcType::TYPED_ROW_ACK ||
        resp.payload_len != 1) {
        release(fd, false);
        return false;
    }
    uint8_t status = 0;
    if (!recv_all(fd, &status, 1)) {
        release(fd, false);
        return false;
    }
    release(fd, true);
    return status == 1u;
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
    if (resp.magic != 0x41504558u ||
        static_cast<RpcType>(resp.type) != RpcType::WAL_ACK ||
        resp.payload_len != 1) {
        release(fd, false);
        return false;
    }
    uint8_t status = 0;
    if (!recv_all(fd, &status, 1)) {
        release(fd, false);
        return false;
    }
    release(fd, true);
    return status == 1u;
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
        resp.magic != 0x41504558u ||
        static_cast<RpcType>(resp.type) != RpcType::STATS_RESULT ||
        resp.payload_len > max_response_size_) {
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
        resp.magic != 0x41504558u ||
        static_cast<RpcType>(resp.type) != RpcType::METRICS_RESULT ||
        resp.payload_len > max_response_size_) {
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

std::vector<uint8_t> TcpRpcClient::request_binary(
    RpcType request_type,
    RpcType response_type,
    const std::vector<uint8_t>& payload,
    bool include_epoch,
    std::string* error) {
    int fd = acquire();
    if (fd < 0) {
        if (error) {
            *error = "TcpRpcClient: cannot connect to "
                + host_ + ":" + std::to_string(port_);
        }
        return {};
    }

    if (query_timeout_ms_ > 0) {
        struct timeval tv;
        tv.tv_sec  = query_timeout_ms_ / 1000;
        tv.tv_usec = (query_timeout_ms_ % 1000) * 1000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    RpcHeader hdr{};
    hdr.type        = static_cast<uint32_t>(request_type);
    hdr.request_id  = 1;
    hdr.payload_len = static_cast<uint32_t>(payload.size());
    hdr.epoch       = include_epoch ? epoch_ : 0;
    if (!send_all(fd, &hdr, sizeof(hdr)) ||
        (!payload.empty() && !send_all(fd, payload.data(), payload.size()))) {
        release(fd, false);
        if (error) *error = "TcpRpcClient: send failed";
        return {};
    }

    RpcHeader resp{};
    if (!recv_all(fd, &resp, sizeof(resp))) {
        release(fd, false);
        if (error) {
            *error = (errno == EAGAIN || errno == EWOULDBLOCK)
                ? "TcpRpcClient: request timeout (" + std::to_string(query_timeout_ms_) + "ms)"
                : "TcpRpcClient: recv header failed";
        }
        return {};
    }

    if (resp.magic != 0x41504558u ||
        static_cast<RpcType>(resp.type) != response_type ||
        resp.payload_len > max_response_size_) {
        release(fd, false);
        if (error) *error = "TcpRpcClient: unexpected response type";
        return {};
    }

    std::vector<uint8_t> response(resp.payload_len);
    if (resp.payload_len > 0 &&
        !recv_all(fd, response.data(), resp.payload_len)) {
        release(fd, false);
        if (error) *error = "TcpRpcClient: recv payload failed";
        return {};
    }
    release(fd, true);
    return response;
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
    bool ok = recv_all(fd, &resp, sizeof(resp)) &&
              resp.magic == 0x41504558u &&
              static_cast<RpcType>(resp.type) == RpcType::PONG &&
              resp.payload_len == 0;
    release(fd, ok);
    return ok;
}

} // namespace zeptodb::cluster
