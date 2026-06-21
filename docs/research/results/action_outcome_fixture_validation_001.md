# Action-Outcome Fixture Validation 001

Generated at: 2026-06-14T00:45:34Z
Fixture: `docs/research/fixtures/action_outcome_episodes.json`

## Summary

- Status: pass
- Episodes: 24
- Incident families: 6
- Minimum family size: 4

## Incident Family Counts

| Incident Type | Episodes |
| --- | ---: |
| checkout_latency_after_deploy | 4 |
| inventory_db_connection_pool | 4 |
| order_queue_backlog | 4 |
| payment_dependency_timeout | 4 |
| recommendation_cache_stale | 4 |
| search_index_memory_leak | 4 |

## Action Class Counts

| Action Class | Episodes |
| --- | ---: |
| cache_purge | 1 |
| config_revert | 2 |
| queue_reset | 1 |
| restart | 6 |
| rollback | 4 |
| scale_out | 6 |
| traffic_drain | 4 |

## Human Outcome Counts

| Outcome | Episodes |
| --- | ---: |
| failure | 6 |
| partial_success | 4 |
| rollback_required | 1 |
| success | 13 |

## Errors

- None.

## Next Steps

1. Use validated fixtures in the context-gated outcome replay.
2. Map the fixture into ZeptoDB SQL tables for replay through the database.
3. Add real or lab-generated incident traces to replace synthetic-only claims.
