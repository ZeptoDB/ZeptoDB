// ============================================================================
// Layer 2: WAL Writer Implementation
// ============================================================================

#include "zeptodb/ingestion/wal_writer.h"
#include "zeptodb/common/logger.h"
#include <stdexcept>

namespace zeptodb::ingestion {

WALWriter::WALWriter(const std::string& path)
    : file_(path, std::ios::binary | std::ios::app)
{
    if (!file_.is_open()) {
        throw std::runtime_error("WALWriter: cannot open " + path);
    }
    ZEPTO_INFO("WAL opened: {}", path);
}

WALWriter::~WALWriter() {
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
}

bool WALWriter::write(const TickMessage& msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    file_.write(reinterpret_cast<const char*>(&msg), sizeof(TickMessage));
    if (!file_.good()) {
        ZEPTO_ERROR("WAL write failed at message #{}", count_);
        return false;
    }
    ++count_;
    return true;
}

void WALWriter::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    file_.flush();
}

} // namespace zeptodb::ingestion
