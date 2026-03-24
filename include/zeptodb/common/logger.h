#pragma once
// ============================================================================
// ZeptoDB Logger (spdlog wrapper)
// ============================================================================

#include <spdlog/spdlog.h>
#include <memory>

namespace zeptodb {

class Logger {
public:
    static void init(const std::string& name = "zeptodb",
                     spdlog::level::level_enum level = spdlog::level::info);

    static std::shared_ptr<spdlog::logger>& get();

private:
    static std::shared_ptr<spdlog::logger> s_logger;
};

// Convenience macros
#define APEX_TRACE(...)    SPDLOG_LOGGER_TRACE(zeptodb::Logger::get(), __VA_ARGS__)
#define ZEPTO_DEBUG(...)    SPDLOG_LOGGER_DEBUG(zeptodb::Logger::get(), __VA_ARGS__)
#define ZEPTO_INFO(...)     SPDLOG_LOGGER_INFO(zeptodb::Logger::get(), __VA_ARGS__)
#define ZEPTO_WARN(...)     SPDLOG_LOGGER_WARN(zeptodb::Logger::get(), __VA_ARGS__)
#define ZEPTO_ERROR(...)    SPDLOG_LOGGER_ERROR(zeptodb::Logger::get(), __VA_ARGS__)
#define APEX_CRITICAL(...) SPDLOG_LOGGER_CRITICAL(zeptodb::Logger::get(), __VA_ARGS__)

} // namespace zeptodb
