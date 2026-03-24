// ============================================================================
// ZeptoDB Logger Implementation
// ============================================================================

#include "zeptodb/common/logger.h"
#include <spdlog/sinks/stdout_color_sinks.h>

namespace zeptodb {

std::shared_ptr<spdlog::logger> Logger::s_logger;

void Logger::init(const std::string& name, spdlog::level::level_enum level) {
    s_logger = spdlog::stdout_color_mt(name);
    s_logger->set_level(level);
    s_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%f] [%^%l%$] [%s:%#] %v");
    SPDLOG_LOGGER_INFO(s_logger, "ZeptoDB Logger initialized");
}

std::shared_ptr<spdlog::logger>& Logger::get() {
    if (!s_logger) {
        init();
    }
    return s_logger;
}

} // namespace zeptodb
