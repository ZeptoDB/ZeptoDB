#include <gtest/gtest.h>

#include "zeptodb/util/logger.h"

TEST(StructuredLogger, FallsBackToStdoutWhenLogDirectoryIsUnavailable) {
    auto& logger = zeptodb::util::Logger::instance();

    EXPECT_NO_THROW({
        logger.init("/proc/zeptodb-logger-unwritable",
                    zeptodb::util::LogLevel::INFO);
        logger.info("stdout fallback is active", "test");
        logger.flush();
    });
}
