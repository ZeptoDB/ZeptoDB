# Devlog 017: Connection Hooks, s# Sorted Index, `\t` CLI Timer

**Date:** 2026-03-23
**Status:** Completed
**Tests:** 467/467 pass (added 20 new tests)

---

## Overview

Implemented three kdb+ parity features that were flagged as production-critical:

1. **Connection hooks** (`.z.po`/`.z.pc`) — session lifecycle callbacks in `HttpServer`
2. **s# attribute hint** — sorted column binary-search index in `Partition`
3. **`\t <sql>` one-shot timer** — interactive query timing in zepto-cli

---

## 1. Connection Hooks (`.z.po` / `.z.pc`)

### Problem

kdb+ users rely heavily on `.z.po`/`.z.pc` for:
- Audit logging (who connected, when)
- Connection rate limiting
- Session-level resource cleanup
- Operations dashboards

ZeptoDB had no equivalent — connections were anonymous.

### Design

Since httplib operates at the HTTP layer (not raw TCP), true socket-level connection events are not available. Instead, we track "logical sessions" by `remote_addr`:

- **on_connect**: fires on the first HTTP request from a new `remote_addr`
- **on_disconnect**: fires when a `Connection: close` header is detected, or when `evict_idle_sessions()` removes a stale session

This is the correct semantic for HTTP/1.1: a client holding a keep-alive connection is "connected"; a `Connection: close` signals intent to close.

### Implementation

```cpp
// http_server.h additions
struct ConnectionInfo {
    std::string remote_addr;
    std::string user;
    int64_t     connected_at_ns;
    int64_t     last_active_ns;
    uint64_t    query_count;
};

void set_on_connect(std::function<void(const ConnectionInfo&)> fn);
void set_on_disconnect(std::function<void(const ConnectionInfo&)> fn);
std::vector<ConnectionInfo> list_sessions() const;
size_t evict_idle_sessions(int64_t timeout_ms);
```

The hook uses `svr_->set_logger(...)` — httplib's post-response callback — to call `track_session(remote_addr, is_closing)` after every request.

`track_session` is mutex-guarded and:
1. Inserts new session + fires `on_connect_` (lock released before callback)
2. Updates `last_active_ns` + `query_count` for returning sessions
3. Removes session + fires `on_disconnect_` when `Connection: close` detected

`evict_idle_sessions` scans the map, removes expired entries, and fires `on_disconnect_` for each — useful for EOD cleanup or leak prevention.

### REST Endpoint

`GET /admin/sessions` returns the active session list (admin-only):
```json
[{"remote_addr":"127.0.0.1:54321","user":"127.0.0.1:54321",
  "connected_at_ns":1742000000000000000,"last_active_ns":1742000001000000000,
  "query_count":3}]
```

### Tests (7)

`OnConnectFiresOnFirstRequest`, `OnConnectFiresOnlyOnce`, `OnDisconnectFiresOnConnectionClose`,
`ListSessionsReturnsActiveSession`, `EvictIdleSessionsFiresOnDisconnect`,
`EvictIdleSessionsKeepsRecentSessions`, `QueryCountIncrements`

---

## 2. s# Sorted Column Attribute Hint

### Problem

kdb+'s `s#` attribute marks a column as sorted, enabling binary search (`bin`) instead of linear scan for range queries. Without this, ZeptoDB scanned all N rows even when the answer was in a narrow range.

For a quant running `WHERE price BETWEEN 15000 AND 16000` on a 1M-row partition with 1% selectivity, this meant scanning 1,000,000 rows to return 10,000.

### Design

Timestamps are always sorted (append-only guarantee), so `timestamp_range()` already existed. We generalize this to arbitrary columns:

```
Partition::set_sorted(col)              → inserts col into sorted_columns_ set
Partition::is_sorted(col)               → O(1) lookup
Partition::sorted_range(col, lo, hi)    → std::lower_bound + std::upper_bound → [begin, end)
```

The executor's `exec_simple_select` calls the new `extract_sorted_col_range()` helper which scans the WHERE AST for:
- `BETWEEN lo AND hi` on a sorted column
- `col >= lo AND col <= hi` (any combination of `>=`, `>`, `<=`, `<`, `=`)

It collects the tightest bounds across all AND-connected predicates, then calls `sorted_range()` and uses `eval_where_ranged()` to evaluate remaining predicates only within the pruned row range.

### Performance Impact

| Scenario | Without s# | With s# | Speedup |
|----------|-----------|---------|---------|
| 1M rows, 1% selectivity | 1,000,000 rows scanned | ~10,000 rows scanned | ~100x |
| 1M rows, 0.1% selectivity | 1,000,000 rows scanned | ~1,000 rows scanned | ~1000x |

This is in addition to the existing XBAR sorted-scan optimization (which reduced GROUP BY XBAR from 45ms → 10ms by caching hash keys for monotonic timestamps).

### Limitations

- `exec_agg` and `exec_agg_parallel` (aggregate-only queries) do not yet use `sorted_range`
- OR / NOT conditions cannot be optimized with a single contiguous range
- `g#` (hash index) and `p#` (parted index) are planned but not implemented

### Tests (13)

`DefaultNotSorted`, `SetAndCheckSorted`, `SortedRangeFullSpan`, `SortedRangeMiddle`,
`SortedRangeExactMatch`, `SortedRangeBelowAll`, `SortedRangeAboveAll`, `SortedRangeUnknownColumn`,
`BetweenOnSortedColumn`, `GELEOnSortedColumn`, `EQOnSortedColumn`, `OutOfRangeOnSortedColumn`,
`RowsScannedReduced`

---

## 3. `\t <sql>` One-Shot Timer

### Problem

kdb+ users type `\t select ...` to time a query without committing to a persistent toggle. In ZeptoDB, `\t` only toggled timing ON/OFF — there was no one-shot syntax.

### Implementation

Two-line addition in `BuiltinCommands::handle()`:

```cpp
if (cmd.size() > 3 && cmd.substr(0, 3) == "\\t ") {
    std::string sql = cmd.substr(3);
    bool saved = cfg_.timing;
    cfg_.timing = true;
    execute_query(sql);
    cfg_.timing = saved;   // restore toggle state
    r.handled = true;
    return r;
}
```

The toggle state is saved before and restored after, so `\t <sql>` is truly non-destructive.

---

## Files Changed

| File | Change |
|------|--------|
| `include/zeptodb/server/http_server.h` | `ConnectionInfo`, `set_on_connect`, `set_on_disconnect`, `list_sessions`, `evict_idle_sessions`, private session state |
| `src/server/http_server.cpp` | `setup_session_tracking`, `track_session`, `list_sessions`, `evict_idle_sessions`, `/admin/sessions` route |
| `include/zeptodb/storage/partition_manager.h` | `set_sorted`, `is_sorted`, `sorted_range`, `sorted_columns_` |
| `include/zeptodb/sql/executor.h` | `extract_sorted_col_range` declaration |
| `src/sql/executor.cpp` | `extract_sorted_col_range` implementation, `exec_simple_select` integration |
| `tools/zepto-cli.cpp` | `\t <sql>` one-shot timer, updated help text |
| `tests/unit/test_features.cpp` | 20 new tests |
| `tests/CMakeLists.txt` | added `test_features.cpp`, `zepto_server` link |
| `BACKLOG.md` | marked 3 items complete |

---

## Next Steps

- Add `extract_sorted_col_range` to `exec_agg` / `exec_agg_parallel` paths
- Implement `g#` hash index for low-cardinality equality lookups
- Implement `p#` parted attribute for grouped columnar layout
- Add `user` field population from JWT/API key subject in `track_session`
