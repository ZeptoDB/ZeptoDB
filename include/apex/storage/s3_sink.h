#pragma once
// ============================================================================
// Layer 1: HDB S3 Sink — Parquet/Binary 파일 S3 업로드
// ============================================================================
// AWS SDK C++ 또는 경량 REST 방식으로 S3에 HDB 파일 업로드.
//
// 설계 원칙:
//   - 로컬 파일 → S3 (PutObject / Multipart 자동 선택)
//   - in-memory buffer → S3 (로컬 임시 파일 없이 직접 스트리밍)
//   - 비동기 업로드: 인제스션 핫패스 비차단
//   - 파티션 경로 규칙: s3://{bucket}/{prefix}/{symbol}/{hour}.parquet
//
// 의존성: aws-sdk-cpp (S3 컴포넌트)
//   Amazon Linux 2023: sudo dnf install -y aws-sdk-cpp-s3
// ============================================================================

#include "apex/common/logger.h"
#include "apex/common/types.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>

// AWS SDK 가용성
#if __has_include(<aws/s3/S3Client.h>)
    #include <aws/core/Aws.h>
    #include <aws/s3/S3Client.h>
    #include <aws/s3/model/PutObjectRequest.h>
    #define APEX_S3_AVAILABLE 1
#else
    #define APEX_S3_AVAILABLE 0
#endif

namespace apex::storage {

// ============================================================================
// S3SinkConfig
// ============================================================================
struct S3SinkConfig {
    std::string bucket;                          // S3 버킷명 (필수)
    std::string prefix       = "hdb";            // S3 키 프리픽스 (예: "apex-db/hdb")
    std::string region       = "us-east-1";      // AWS 리전
    std::string endpoint_url = "";               // MinIO 등 custom endpoint ("" = AWS 기본)
    bool        use_path_style = false;          // MinIO: true, AWS S3: false
    size_t      multipart_threshold = 64ULL * 1024 * 1024; // 64MB 초과 시 Multipart
};

// ============================================================================
// S3Sink: 파일/버퍼 → S3 업로드
// ============================================================================
class S3Sink {
public:
    explicit S3Sink(S3SinkConfig config);
    ~S3Sink();

    // Non-copyable
    S3Sink(const S3Sink&) = delete;
    S3Sink& operator=(const S3Sink&) = delete;

    /// 로컬 파일 → S3 동기 업로드
    /// @param local_path  업로드할 로컬 파일 경로
    /// @param s3_key      S3 오브젝트 키 (config.prefix 제외)
    /// @return true on success
    bool upload_file(const std::string& local_path, const std::string& s3_key);

    /// in-memory 버퍼 → S3 동기 업로드 (임시 파일 없음)
    /// @param data  업로드할 데이터
    /// @param size  데이터 크기 (bytes)
    /// @param s3_key S3 오브젝트 키
    bool upload_buffer(const char* data, size_t size, const std::string& s3_key);

    /// 로컬 파일 → S3 비동기 업로드 (fire-and-forget)
    /// @return future<bool> — 완료 확인용
    std::future<bool> upload_file_async(const std::string& local_path,
                                        const std::string& s3_key);

    /// 파티션 키로 S3 오브젝트 키 생성
    /// 형식: {prefix}/{symbol}/{hour_epoch}.parquet
    std::string make_s3_key(SymbolId symbol, int64_t hour_epoch,
                             const std::string& ext = "parquet") const;

    /// 업로드된 오브젝트의 S3 URI 반환
    std::string make_s3_uri(const std::string& s3_key) const;

    // --- 통계 ---
    [[nodiscard]] size_t uploads_succeeded() const {
        return uploads_succeeded_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] size_t uploads_failed() const {
        return uploads_failed_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] size_t bytes_uploaded() const {
        return bytes_uploaded_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] static bool s3_available() {
        return APEX_S3_AVAILABLE == 1;
    }

    [[nodiscard]] const S3SinkConfig& config() const { return config_; }

private:
#if APEX_S3_AVAILABLE
    std::shared_ptr<Aws::S3::S3Client> client_;
    bool aws_sdk_initialized_ = false;
    void init_aws_sdk();
    void shutdown_aws_sdk();
#endif

    S3SinkConfig config_;

    std::atomic<size_t> uploads_succeeded_{0};
    std::atomic<size_t> uploads_failed_{0};
    std::atomic<size_t> bytes_uploaded_{0};
};

} // namespace apex::storage
