#pragma once
// ============================================================================
// ZeptoDB: 구조화된 로깅 시스템
// ============================================================================
// 프로덕션 레벨 로깅:
// - JSON 형식 (구조화)
// - 로그 레벨 (TRACE, DEBUG, INFO, WARN, ERROR, CRITICAL)
// - 자동 로테이션 (일별, 크기 기반)
// - 성능 최적화 (비동기 로깅)
// ============================================================================

#include <string>
#include <memory>
#include <sstream>

namespace zeptodb::util {

// ============================================================================
// 로그 레벨
// ============================================================================
enum class LogLevel {
    TRACE = 0,
    DEBUG = 1,
    INFO = 2,
    WARN = 3,
    ERROR = 4,
    CRITICAL = 5,
    OFF = 6
};

// ============================================================================
// Logger: 싱글톤 구조화된 로거
// ============================================================================
class Logger {
public:
    static Logger& instance();

    // 초기화 (파일 경로, 레벨, 로테이션 설정)
    void init(const std::string& log_dir = "/var/log/zeptodb",
              LogLevel level = LogLevel::INFO,
              size_t max_file_size_mb = 100,
              size_t max_files = 10);

    // 로그 레벨 변경
    void set_level(LogLevel level);
    LogLevel get_level() const;

    // JSON 구조화된 로그 출력
    void log(LogLevel level, const std::string& message,
             const std::string& component = "",
             const std::string& details = "");

    // 편의 함수
    void trace(const std::string& msg, const std::string& component = "");
    void debug(const std::string& msg, const std::string& component = "");
    void info(const std::string& msg, const std::string& component = "");
    void warn(const std::string& msg, const std::string& component = "");
    void error(const std::string& msg, const std::string& component = "");
    void critical(const std::string& msg, const std::string& component = "");

    // 플러시 (비동기 버퍼 강제 기록)
    void flush();

    ~Logger();

private:
    Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    class Impl;
    std::unique_ptr<Impl> impl_;
};

// ============================================================================
// 전역 로거 매크로 (편의성)
// ============================================================================
#define LOG_TRACE(msg, ...) \
    ::zeptodb::util::Logger::instance().trace(msg, ##__VA_ARGS__)
#define LOG_DEBUG(msg, ...) \
    ::zeptodb::util::Logger::instance().debug(msg, ##__VA_ARGS__)
#define LOG_INFO(msg, ...) \
    ::zeptodb::util::Logger::instance().info(msg, ##__VA_ARGS__)
#define LOG_WARN(msg, ...) \
    ::zeptodb::util::Logger::instance().warn(msg, ##__VA_ARGS__)
#define LOG_ERROR(msg, ...) \
    ::zeptodb::util::Logger::instance().error(msg, ##__VA_ARGS__)
#define LOG_CRITICAL(msg, ...) \
    ::zeptodb::util::Logger::instance().critical(msg, ##__VA_ARGS__)

} // namespace zeptodb::util
