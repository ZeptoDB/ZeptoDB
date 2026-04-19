# 089 — Client hardening: identifier validation, SQL escape, CLI semicolons

Date: 2026-04-18
Status: Complete
Related: [088](088_client_ddl_and_final_residuals.md)

## Scope

Six residual items surfaced during the devlog-088 review cycle. All are
small surface fixes — together they close the last weak spots in the
Python HTTP client, the interactive CLI, the Binance feed comments, and
the aarch64 runner script.

| # | Residual (from 088 review) | Resolution in 089 |
|---|---|---|
| 1 | Python DDL helpers interpolated caller-supplied names into SQL with no validation → trivial injection surface | **F1** — `_validate_identifier` / `_validate_type` helpers, wired into `create_table`, `drop_table`, `ingest_pandas`, `ingest_polars` (`zepto_py/connection.py:117-139`) |
| 2 | `ingest_pandas` emitted raw `f"'{val}'"` for string values → any embedded `'` broke the statement or enabled injection | **F2** — double embedded `'` per SQL standard; tokenizer also taught to decode `''` back to `'` (`zepto_py/connection.py:325`, `src/sql/tokenizer.cpp:160-179`) |
| 3 | Interactive CLI accumulated until `;`, then sent the full string incl. `;`; server tokenizer rejected `;` | **F3** — strip trailing `;` (and whitespace) right before `execute_query(sql)` in the REPL and the `run_script` paths (`tools/zepto-cli.cpp:666-680`, `tools/zepto-cli.cpp:707-717`) |
| 4 | `parse_trade_message` / `parse_depth_message` promoted to public in 088 but no `TODO` marker for the eventual demotion | **F4** — explicit `TODO(devlog 088)` comment next to the declarations (`include/zeptodb/feeds/binance_feed.h:55-56`) |
| 5 | `tick.volume = static_cast<uint64_t>(std::stod(qty_s))` silently truncates sub-unit Binance quantities (`0.001 → 0`) | **F5** — inline `NOTE` documenting truncation semantics and the scale-factor workaround (`src/feeds/binance_feed.cpp:115-118`) |
| 6 | `run-aarch64-tests.sh` default `REPO=REPLACE_ME...` would push a broken tag if operator forgot to override | **F6** — early guard: exit 1 if `REPO` still contains `REPLACE_ME` and `SKIP_PUSH != 1` (`tools/run-aarch64-tests.sh:27-31`) |

## F1 — Python SQL identifier validation

New private helpers on `ZeptoConnection`:

```python
@staticmethod
def _validate_identifier(name: str) -> str:
    if not re.match(r'^[A-Za-z_][A-Za-z0-9_]*$', name):
        raise ValueError(f"Invalid SQL identifier: {name!r}")
    return name

@staticmethod
def _validate_type(name: str) -> str:
    if not re.match(r'^[A-Za-z0-9_]+$', name):
        raise ValueError(f"Invalid SQL type: {name!r}")
    return name
```

Call-site wiring:

* `create_table` — validates `name`; every `(col_name, col_type)` pair is
  run through `_validate_identifier(col_name)` and `_validate_type(col_type)`
  respectively. Type strings are a controlled set (`INT64`, `DOUBLE`,
  `SYMBOL`, ...) but we still reject anything outside `[A-Za-z0-9_]+` so
  a caller can't sneak a `'; DROP` in via a typo.
* `drop_table` — validates `name`.
* `ingest_pandas` — validates `table_name`.
* `ingest_polars` — validates `table_name` (before the polars→pandas
  conversion, so bad names fail fast without touching the DataFrame).

## F2 — `ingest_pandas` string escape

Old: `row_vals.append(f"'{val}'")` — a value like `it's` produced
`'it's'`, which the tokenizer read as `'it'` + identifier `s` +
string-start `'`...`)` = unterminated literal.

New: `row_vals.append("'" + val.replace("'", "''") + "'")` — standard SQL
single-quote doubling.

The tokenizer was previously oblivious to `''`:

```cpp
// src/sql/tokenizer.cpp
while (!at_end() && peek() != '\'') s += advance();
```

It now recognizes `''` as a literal `'`:

```cpp
while (!at_end()) {
    if (peek() == '\'') {
        if (pos_ + 1 < sql_.size() && sql_[pos_ + 1] == '\'') {
            s += '\'';          // devlog 089: '' == one '
            advance(); advance();
            continue;
        }
        break;                  // terminating quote
    }
    s += advance();
}
```

This is mandatory for F2 to round-trip: the client produces `''` and the
server must decode it back to `'` before handing the value to the
executor.

## F3 — CLI trailing-semicolon strip

`handle()` already trimmed `;` for the internal command dispatcher, but
the non-command path (ordinary SQL) forwarded the accumulated buffer
verbatim to `cmds_.execute_query(accumulated)`. The server's tokenizer
has never accepted `;`, so `CREATE TABLE t (a INT64);` typed at the
prompt returned `Tokenizer: unexpected character ';'`.

Fix: right before the dispatch, strip trailing `;` and whitespace:

```cpp
std::string sql = accumulated;
while (!sql.empty() && sql.back() == ';') sql.pop_back();
while (!sql.empty() && (sql.back()==' '||sql.back()=='\t'||
                        sql.back()=='\n'||sql.back()=='\r'))
    sql.pop_back();
cmds_.execute_query(sql);
```

Applied twice — once in the REPL loop and once in the `run_script`
path (the script reader has the same `;`-as-delimiter convention).

Manually verified against a live server:

```
$ ./build/zepto_http_server --port 18765 --no-auth &
$ echo 'SHOW TABLES;' | ./build/zepto-cli --host localhost --port 18765
...
+------+-------+
| name | rows  |
+------+-------+
| t1   | 10000 |
+------+-------+
1 row in set
```

Previously this would have returned a tokenizer error on the trailing `;`.

## F4 — Binance public → TODO comment

The devlog-088 decision to promote `parse_trade_message` /
`parse_depth_message` to `public:` was spelled out in the devlog but
there was no in-tree marker reminding the next developer to demote them
once the WebSocket transport lands. Added an explicit
`TODO(devlog 088):` next to the declarations.

## F5 — Binance qty precision note

`static_cast<uint64_t>(std::stod("0.001"))` silently evaluates to `0`.
For integer-volume schemas (share count, order count) this is fine.
For sub-unit Binance quantities (crypto, FX) it isn't. Added an inline
`NOTE` pointing to the documented workaround (pre-scale by a factor
before cast) and the backlog entry for a proper precision mode.

## F6 — `run-aarch64-tests.sh` REPO guard

Default value is intentionally
`REPO=REPLACE_ME.dkr.ecr.ap-northeast-2.amazonaws.com/zeptodb` so the
script doesn't accidentally default-push to a real repo. But `buildx
--push` would still attempt a DNS resolution on the placeholder host and
fail with an opaque error. Early guard:

```bash
if [[ "$REPO" == *REPLACE_ME* ]] && [[ "${SKIP_PUSH:-0}" != "1" ]]; then
    echo "ERROR: REPO still contains REPLACE_ME. Set REPO=<your-ecr-repo> or SKIP_PUSH=1."
    exit 1
fi
```

Fails the run immediately with a clear message. `SKIP_PUSH=1` continues
to work for local smoke builds that don't push.

## Files changed

| File | Change |
|---|---|
| `zepto_py/connection.py` | F1: `_validate_identifier` / `_validate_type` helpers + wiring. F2: escape single quotes in string values. |
| `src/sql/tokenizer.cpp` | F2: recognize `''` as literal `'` inside string literals. |
| `tools/zepto-cli.cpp` | F3: strip trailing `;` in REPL and `run_script` before dispatch. |
| `include/zeptodb/feeds/binance_feed.h` | F4: `TODO(devlog 088)` on the public parse methods. |
| `src/feeds/binance_feed.cpp` | F5: `NOTE` on qty truncation semantics. |
| `tools/run-aarch64-tests.sh` | F6: `REPLACE_ME` guard. |
| `tests/python/test_client_ddl.py` | +5 tests (invalid table/column/drop/ingest-table-name, embedded-quote ingest). |
| `docs/devlog/088_client_ddl_and_final_residuals.md` | residuals section — all 6 marked RESOLVED → 089. |
| `docs/api/PYTHON_REFERENCE.md` | note that identifiers are validated (ValueError on invalid names). |
| `docs/COMPLETED.md` | bullet for devlog 089. |
| `.kiro/KIRO.md` | next-devlog hint 089 → 090. |

## Tests added

| Test | Purpose |
|---|---|
| `test_invalid_table_name_rejected` | F1: `create_table` rejects injection in `name`. |
| `test_invalid_column_name_rejected` | F1: `create_table` rejects injection in column name. |
| `test_invalid_drop_table_name_rejected` | F1: `drop_table` rejects injection. |
| `test_invalid_ingest_pandas_table_name_rejected` | F1: `ingest_pandas` rejects injection in `table_name`. |
| `test_ingest_pandas_string_with_quotes` | F2: round-trip DataFrame with embedded `'` and `"` survives escape + tokenizer decode. |

## Verification

```
$ ./tests/zepto_tests 2>&1 | tail -3
[==========] 1210 tests from 152 test suites ran.
[  PASSED  ] 1210 tests.        # unchanged

$ cd tests/python && python3 -m pytest 2>&1 | tail -1
======================== 219 passed, 1 warning in 6.64s ========================
                                # was 214, +5 (F1 × 4, F2 × 1)

$ echo 'SHOW TABLES;' | ./build/zepto-cli --host localhost --port 18765
# returns formatted result table, not a tokenizer error
```

x86_64 baseline on the dev host. No platform-specific code touched —
all 6 fixes are arch-neutral.

## 088 residual closure

All six items from the devlog-088 review are now RESOLVED. The only
remaining deferred piece is the aarch64 EKS bench run itself
(requires ECR credentials that this environment doesn't have) — the
guard from F6 makes that run fail fast when the operator forgets to
set `REPO`.
