// ============================================================================
// ZeptoDB: 구조화된 로깅 시스템 구현
// ============================================================================
// spdlog 기반 비동기 JSON 로깅
// ============================================================================

#include "zeptodb/util/logger.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/async.h>
#include <chrono>
#include <iomanip>
#include <filesystem>

namespace zeptodb::util {

namespace fs = std::filesystem;

// ============================================================================
// Logger::Impl — spdlog 래퍼
// ============================================================================
class Logger::Impl {
public:
    std::shared_ptr<spdlog::logger> logger_;
    LogLevel current_level_ = LogLevel::INFO;

    void init(const std::string& log_dir,
              LogLevel level,
              size_t max_file_size_mb,
              size_t max_files)
    {
        current_level_ = level;

        // 로그 디렉토리 생성
        fs::create_directories(log_dir);

        // 파일 경로
        std::string log_path = log_dir + "/zeptodb.log";

        // Rotating file sink (자동 로테이션)
        size_t max_size = max_file_size_mb * 1024 * 1024;
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            log_path, max_size, max_files);

        // Stdout sink (콘솔 출력)
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

        // 비동기 로거 생성
        spdlog::init_thread_pool(8192, 1);  // 큐 크기 8192, 스레드 1개
        std::vector<spdlog::sink_ptr> sinks{file_sink, console_sink};
        logger_ = std::make_shared<spdlog::async_logger>(
            "zeptodb", sinks.begin(), sinks.end(),
            spdlog::thread_pool(),
            spdlog::async_overflow_policy::block);

        // 패턴 설정 (JSON 형식)
        logger_->set_pattern("%v");

        // spdlog 레벨 매핑
        logger_->set_level(to_spdlog_level(level));

        spdlog::drop("zeptodb");
        spdlog::register_logger(logger_);
    }

    static spdlog::level::level_enum to_spdlog_level(LogLevel level) {
        switch (level) {
            case LogLevel::TRACE:    return spdlog::level::trace;
            case LogLevel::DEBUG:    return spdlog::level::debug;
            case LogLevel::INFO:     return spdlog::level::info;
            case LogLevel::WARN:     return spdlog::level::warn;
            case LogLevel::ERROR:    return spdlog::level::err;
            case LogLevel::CRITICAL: return spdlog::level::critical;
            case LogLevel::OFF:      return spdlog::level::off;
        }
        return spdlog::level::info;
    }

    static const char* level_to_string(LogLevel level) {
        switch (level) {
            case LogLevel::TRACE:    return "TRACE";
            case LogLevel::DEBUG:    return "DEBUG";
            case LogLevel::INFO:     return "INFO";
            case LogLevel::WARN:     return "WARN";
            case LogLevel::ERROR:    return "ERROR";
            case LogLevel::CRITICAL: return "CRITICAL";
            case LogLevel::OFF:      return "OFF";
        }
        return "UNKNOWN";
    }

    // JSON 로그 메시지 생성
    std::string build_json_log(LogLevel level,
                                const std::string& message,
                                const std::string& component,
                                const std::string& details)
    {
        // ISO 8601 타임스탬프
        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;
        auto timer = std::chrono::system_clock::to_time_t(now);
        std::tm tm;
        localtime_r(&timer, &tm);

        std::ostringstream os;
        os << "{"
           << "\"timestamp\":\"";
        os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
        os << '.' << std::setfill('0') << std::setw(3) << ms.count();
        os << std::put_time(&tm, "%z") << "\",";

        os << "\"level\":\"" << level_to_string(level) << "\",";
        os << "\"message\":\"" << escape_json(message) << "\"";

        if (!component.empty()) {
            os << ",\"component\":\"" << escape_json(component) << "\"";
        }

        if (!details.empty()) {
            os << ",\"details\":\"" << escape_json(details) << "\"";
        }

        os << "}";
        return os.str();
    }

    // JSON 이스케이프
    static std::string escape_json(const std::string& str) {
        std::string escaped;
        for (char c : str) {
            switch (c) {
                case '"':  escaped += "\\\""; break;
                case '\\': escaped += "\\\\"; break;
                case '\n': escaped += "\\n"; break;
                case '\r': escaped += "\\r"; break;
                case '\t': escaped += "\\t"; break;
                default:   escaped += c; break;
            }
        }
        return escaped;
    }
};

// ============================================================================
// Logger 싱글톤
// ============================================================================
Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::init(const std::string& log_dir,
                  LogLevel level,
                  size_t max_file_size_mb,
                  size_t max_files)
{
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
    }
    impl_->init(log_dir, level, max_file_size_mb, max_files);
}

void Logger::set_level(LogLevel level) {
    if (impl_) {
        impl_->current_level_ = level;
        impl_->logger_->set_level(Impl::to_spdlog_level(level));
    }
}

LogLevel Logger::get_level() const {
    return impl_ ? impl_->current_level_ : LogLevel::INFO;
}

void Logger::log(LogLevel level,
                 const std::string& message,
                 const std::string& component,
                 const std::string& details)
{
    if (!impl_ || level < impl_->current_level_) return;

    std::string json_log = impl_->build_json_log(level, message, component, details);

    switch (level) {
        case LogLevel::TRACE:
            impl_->logger_->trace(json_log);
            break;
        case LogLevel::DEBUG:
            impl_->logger_->debug(json_log);
            break;
        case LogLevel::INFO:
            impl_->logger_->info(json_log);
            break;
        case LogLevel::WARN:
            impl_->logger_->warn(json_log);
            break;
        case LogLevel::ERROR:
            impl_->logger_->error(json_log);
            break;
        case LogLevel::CRITICAL:
            impl_->logger_->critical(json_log);
            break;
        default:
            break;
    }
}

void Logger::trace(const std::string& msg, const std::string& component) {
    log(LogLevel::TRACE, msg, component);
}

void Logger::debug(const std::string& msg, const std::string& component) {
    log(LogLevel::DEBUG, msg, component);
}

void Logger::info(const std::string& msg, const std::string& component) {
    log(LogLevel::INFO, msg, component);
}

void Logger::warn(const std::string& msg, const std::string& component) {
    log(LogLevel::WARN, msg, component);
}

void Logger::error(const std::string& msg, const std::string& component) {
    log(LogLevel::ERROR, msg, component);
}

void Logger::critical(const std::string& msg, const std::string& component) {
    log(LogLevel::CRITICAL, msg, component);
}

void Logger::flush() {
    if (impl_ && impl_->logger_) {
        impl_->logger_->flush();
    }
}

Logger::~Logger() {
    if (impl_ && impl_->logger_) {
        impl_->logger_->flush();
        spdlog::drop("zeptodb");
    }
}

} // namespace zeptodb::util
