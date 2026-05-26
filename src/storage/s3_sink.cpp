// ============================================================================
// Layer 1: HDB S3 Sink — 구현
// ============================================================================

#include "zeptodb/storage/s3_sink.h"

#include <array>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>

#if __has_include(<unistd.h>)
    #include <unistd.h>
    #define ZEPTO_HAS_GETHOSTNAME 1
#else
    #define ZEPTO_HAS_GETHOSTNAME 0
#endif

#if ZEPTO_S3_AVAILABLE
    #include <aws/core/auth/AWSCredentialsProviderChain.h>
    #include <aws/s3/model/PutObjectRequest.h>
    #include <aws/s3/model/CreateMultipartUploadRequest.h>
    #include <aws/s3/model/UploadPartRequest.h>
    #include <aws/s3/model/CompleteMultipartUploadRequest.h>
#endif

namespace zeptodb::storage {

namespace fs = std::filesystem;

namespace {

// 4-char hex hash of `s` using FNV-1a 32-bit. Stable across processes /
// architectures; only the bottom 16 bits are emitted (4 hex chars).
std::string fnv1a_hex4(std::string_view s) {
    uint32_t h = 0x811C9DC5u;
    for (unsigned char c : s) {
        h ^= c;
        h *= 0x01000193u;
    }
    char buf[5];
    std::snprintf(buf, sizeof(buf), "%04x", static_cast<unsigned>(h & 0xFFFFu));
    return std::string(buf, 4);
}

// Read hostname via POSIX gethostname(). Returns empty string on failure or
// when the platform lacks gethostname.
std::string read_hostname() {
#if ZEPTO_HAS_GETHOSTNAME
    std::array<char, 256> buf{};
    if (::gethostname(buf.data(), buf.size() - 1) != 0) {
        return {};
    }
    buf[buf.size() - 1] = '\0';
    return std::string(buf.data());
#else
    return {};
#endif
}

} // namespace

// ============================================================================
// 생성자 / 소멸자
// ============================================================================
S3Sink::S3Sink(S3SinkConfig config)
    : config_(std::move(config))
{
    // devlog 118: compute per-process hostname hash once for HIVE layout
    // filename collision protection. Empty string when disabled or when
    // gethostname() is unavailable / fails.
    if (!config_.disable_host_hash) {
        const std::string host = read_hostname();
        if (!host.empty()) {
            host_hash_ = fnv1a_hex4(host);
        }
    }

#if ZEPTO_S3_AVAILABLE
    init_aws_sdk();
    ZEPTO_INFO("S3Sink 초기화: bucket={}, prefix={}, region={}, layout={}, host_hash={}",
              config_.bucket, config_.prefix, config_.region,
              config_.layout == S3Layout::HIVE ? "hive" : "flat",
              host_hash_.empty() ? "<none>" : host_hash_);
#else
    ZEPTO_WARN("S3Sink: aws-sdk-cpp 없음 — S3 업로드 비활성화");
    ZEPTO_WARN("  설치: sudo dnf install -y aws-sdk-cpp-s3");
#endif
}

S3Sink::~S3Sink()
{
#if ZEPTO_S3_AVAILABLE
    shutdown_aws_sdk();
#endif
}

// ============================================================================
// upload_file: 로컬 파일 → S3
// ============================================================================
bool S3Sink::upload_file(const std::string& local_path, const std::string& s3_key)
{
#if ZEPTO_S3_AVAILABLE
    if (!client_) {
        ZEPTO_WARN("S3Sink::upload_file: 클라이언트 미초기화");
        return false;
    }

    const std::string full_key = config_.prefix.empty()
        ? s3_key
        : config_.prefix + "/" + s3_key;

    // 파일 스트림 열기
    auto input_stream = Aws::MakeShared<Aws::FStream>(
        "S3UploadStream", local_path.c_str(),
        std::ios_base::in | std::ios_base::binary);

    if (!input_stream->is_open()) {
        ZEPTO_WARN("S3Sink: 파일 열기 실패: {}", local_path);
        uploads_failed_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const size_t file_size = static_cast<size_t>(fs::file_size(local_path));

    Aws::S3::Model::PutObjectRequest request;
    request.SetBucket(config_.bucket.c_str());
    request.SetKey(full_key.c_str());
    request.SetBody(input_stream);
    request.SetContentLength(static_cast<long long>(file_size));
    request.SetContentType("application/octet-stream");

    // Parquet 파일임을 메타데이터로 표시
    if (local_path.size() >= 8 &&
        local_path.substr(local_path.size() - 8) == ".parquet") {
        request.SetContentType("application/vnd.apache.parquet");
    }

    auto outcome = client_->PutObject(request);
    if (!outcome.IsSuccess()) {
        ZEPTO_WARN("S3Sink: 업로드 실패: s3://{}/{} ({})",
                  config_.bucket, full_key,
                  outcome.GetError().GetMessage().c_str());
        uploads_failed_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    uploads_succeeded_.fetch_add(1, std::memory_order_relaxed);
    bytes_uploaded_.fetch_add(file_size, std::memory_order_relaxed);

    ZEPTO_INFO("S3 업로드 완료: s3://{}/{} ({}B)",
              config_.bucket, full_key, file_size);
    return true;

#else
    (void)local_path;
    (void)s3_key;
    ZEPTO_WARN("S3Sink: aws-sdk-cpp 없음");
    return false;
#endif
}

// ============================================================================
// upload_buffer: in-memory 버퍼 → S3
// ============================================================================
bool S3Sink::upload_buffer(const char* data, size_t size, const std::string& s3_key)
{
#if ZEPTO_S3_AVAILABLE
    if (!client_ || !data || size == 0) return false;

    const std::string full_key = config_.prefix.empty()
        ? s3_key
        : config_.prefix + "/" + s3_key;

    // stringstream으로 버퍼를 AWS SDK 스트림으로 변환
    auto ss = Aws::MakeShared<Aws::StringStream>("S3BufferStream");
    ss->write(data, static_cast<std::streamsize>(size));

    Aws::S3::Model::PutObjectRequest request;
    request.SetBucket(config_.bucket.c_str());
    request.SetKey(full_key.c_str());
    request.SetBody(ss);
    request.SetContentLength(static_cast<long long>(size));
    request.SetContentType("application/vnd.apache.parquet");

    auto outcome = client_->PutObject(request);
    if (!outcome.IsSuccess()) {
        ZEPTO_WARN("S3Sink::upload_buffer 실패: s3://{}/{} ({})",
                  config_.bucket, full_key,
                  outcome.GetError().GetMessage().c_str());
        uploads_failed_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    uploads_succeeded_.fetch_add(1, std::memory_order_relaxed);
    bytes_uploaded_.fetch_add(size, std::memory_order_relaxed);

    ZEPTO_INFO("S3 버퍼 업로드 완료: s3://{}/{} ({}B)",
              config_.bucket, full_key, size);
    return true;

#else
    (void)data; (void)size; (void)s3_key;
    return false;
#endif
}

// ============================================================================
// upload_file_async: 비동기 업로드
// ============================================================================
std::future<bool> S3Sink::upload_file_async(const std::string& local_path,
                                             const std::string& s3_key)
{
    return std::async(std::launch::async,
                      [this, local_path, s3_key]() {
                          return upload_file(local_path, s3_key);
                      });
}

// ============================================================================
// make_s3_key: 파티션 키 → S3 오브젝트 키
//
// FLAT (default — byte-identical to pre-118 keys):
//   {symbol}/{hour_epoch}.{ext}
//
// HIVE (devlog 118 — Athena/DuckDB/Polars/Spark Hive auto-discovery):
//   year=YYYY/month=MM/day=DD/symbol={ID}/{ID}-{hour_epoch}[-{hash}].{ext}
//
// Year/month/day are derived from `hour_epoch * 3600` seconds via
// `gmtime_r()` so the partitioning is UTC-stable. The optional 4-char
// hostname hash protects against multi-pod filename collisions on the
// same partition; absent when disabled or when gethostname() failed.
// ============================================================================
std::string S3Sink::make_s3_key(SymbolId symbol, int64_t hour_epoch,
                                 const std::string& ext) const
{
    if (config_.layout == S3Layout::FLAT) {
        return std::to_string(symbol) + "/" +
               std::to_string(hour_epoch) + "." + ext;
    }

    // --- HIVE layout ---
    // Convert hour_epoch (hours since 1970-01-01 UTC) to broken-down UTC.
    const std::time_t epoch_seconds = static_cast<std::time_t>(hour_epoch) * 3600;
    std::tm tm_utc{};
    // gmtime_r is the thread-safe POSIX variant; falls back to gmtime if
    // the platform lacks it. Both produce UTC.
#if defined(_POSIX_VERSION) || defined(__linux__)
    ::gmtime_r(&epoch_seconds, &tm_utc);
#else
    if (const std::tm* g = std::gmtime(&epoch_seconds)) tm_utc = *g;
#endif

    // tm_year is years since 1900; tm_mon is 0–11; tm_mday is 1–31.
    char buf[80];
    std::snprintf(buf, sizeof(buf),
                  "year=%04d/month=%02d/day=%02d/symbol=%u/%u-%lld",
                  tm_utc.tm_year + 1900,
                  tm_utc.tm_mon + 1,
                  tm_utc.tm_mday,
                  static_cast<unsigned>(symbol),
                  static_cast<unsigned>(symbol),
                  static_cast<long long>(hour_epoch));

    std::string key(buf);
    if (!host_hash_.empty()) {
        key += "-";
        key += host_hash_;
    }
    key += ".";
    key += ext;
    return key;
}

// ============================================================================
// make_s3_uri: 전체 S3 URI 반환
// ============================================================================
std::string S3Sink::make_s3_uri(const std::string& s3_key) const
{
    if (config_.prefix.empty()) {
        return "s3://" + config_.bucket + "/" + s3_key;
    }
    return "s3://" + config_.bucket + "/" + config_.prefix + "/" + s3_key;
}

// ============================================================================
// AWS SDK 초기화 / 종료
// ============================================================================
#if ZEPTO_S3_AVAILABLE
void S3Sink::init_aws_sdk()
{
    Aws::SDKOptions options;
    Aws::InitAPI(options);
    aws_sdk_initialized_ = true;

    Aws::Client::ClientConfiguration client_config;
    client_config.region = config_.region.c_str();

    if (!config_.endpoint_url.empty()) {
        client_config.endpointOverride = config_.endpoint_url.c_str();
    }

    if (config_.use_path_style) {
        client_ = std::make_shared<Aws::S3::S3Client>(
            client_config,
            Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never,
            /*useVirtualAddressing=*/false);
    } else {
        client_ = std::make_shared<Aws::S3::S3Client>(client_config);
    }
}

void S3Sink::shutdown_aws_sdk()
{
    client_.reset();
    if (aws_sdk_initialized_) {
        Aws::SDKOptions options;
        Aws::ShutdownAPI(options);
        aws_sdk_initialized_ = false;
    }
}
#endif

} // namespace zeptodb::storage
