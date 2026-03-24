// ============================================================================
// ZeptoDB: ParallelScanExecutor Implementation
// ============================================================================

#include "zeptodb/execution/parallel_scan.h"
#include <algorithm>

namespace zeptodb::execution {

std::vector<std::vector<zeptodb::storage::Partition*>>
ParallelScanExecutor::make_partition_chunks(
    const std::vector<zeptodb::storage::Partition*>& parts,
    size_t num_chunks)
{
    if (parts.empty() || num_chunks == 0) return {};
    num_chunks = std::min(num_chunks, parts.size());

    std::vector<std::vector<zeptodb::storage::Partition*>> chunks(num_chunks);
    size_t chunk_size = (parts.size() + num_chunks - 1) / num_chunks;

    for (size_t i = 0; i < parts.size(); ++i) {
        chunks[i / chunk_size].push_back(parts[i]);
    }
    // Remove empty trailing chunks
    chunks.erase(
        std::remove_if(chunks.begin(), chunks.end(),
                       [](const auto& c) { return c.empty(); }),
        chunks.end());
    return chunks;
}

std::vector<std::pair<size_t, size_t>>
ParallelScanExecutor::make_row_chunks(size_t num_rows, size_t num_chunks)
{
    if (num_rows == 0 || num_chunks == 0) return {};
    num_chunks = std::min(num_chunks, num_rows);

    std::vector<std::pair<size_t, size_t>> ranges;
    ranges.reserve(num_chunks);
    size_t chunk_size = (num_rows + num_chunks - 1) / num_chunks;

    for (size_t i = 0; i < num_chunks; ++i) {
        size_t begin = i * chunk_size;
        size_t end   = std::min(begin + chunk_size, num_rows);
        if (begin < end) ranges.emplace_back(begin, end);
    }
    return ranges;
}

ParallelMode ParallelScanExecutor::select_mode(
    size_t num_partitions,
    size_t total_rows,
    size_t num_threads,
    size_t serial_threshold)
{
    if (num_threads <= 1 || total_rows < serial_threshold) {
        return ParallelMode::SERIAL;
    }
    if (num_partitions >= num_threads) {
        return ParallelMode::PARTITION;
    }
    return ParallelMode::CHUNKED;
}

} // namespace zeptodb::execution
