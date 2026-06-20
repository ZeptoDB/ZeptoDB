# Action-Outcome Fixture Validation 001

Generated at: 2026-06-14T00:45:34Z
Fixture: `docs/research/fixtures/action_outcome_distractor_episodes.json`

## Summary

- Status: pass
- Episodes: 8
- Incident families: 5
- Minimum family size: 1

## Incident Family Counts

| Incident Type | Episodes |
| --- | ---: |
| inventory_db_connection_pool | 1 |
| order_queue_backlog | 2 |
| payment_dependency_timeout | 1 |
| recommendation_cache_stale | 2 |
| search_index_memory_leak | 2 |

## Action Class Counts

| Action Class | Episodes |
| --- | ---: |
| cache_purge | 1 |
| restart | 3 |
| rollback | 1 |
| scale_out | 3 |

## Human Outcome Counts

| Outcome | Episodes |
| --- | ---: |
| failure | 2 |
| rollback_required | 1 |
| success | 5 |

## Errors

- None.

## Next Steps

1. Use validated fixtures in the context-gated outcome replay.
2. Map the fixture into ZeptoDB SQL tables for replay through the database.
3. Add real or lab-generated incident traces to replace synthetic-only claims.
