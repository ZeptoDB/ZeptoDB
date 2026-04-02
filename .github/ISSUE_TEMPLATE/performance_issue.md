---
name: Performance Issue
about: Report a performance regression or unexpected slowness
title: "[Perf] "
labels: performance
assignees: ''
---

## Query / Operation

```sql
-- The slow query or operation
```

## Performance Numbers

- Expected: [e.g., <1ms for 1M rows]
- Actual: [e.g., 50ms for 1M rows]
- Dataset size: [rows, columns, data types]

## Environment

- OS / Architecture:
- CPU: [model, cores, NUMA nodes]
- Memory: [total, available]
- Build flags: [Release? LTO? PGO?]
- ZeptoDB version/commit:

## Profiling Data (if available)

`perf`, `vtune`, or `EXPLAIN` output.
