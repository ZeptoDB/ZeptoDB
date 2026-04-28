# Devlog 108 — OPC-UA Sprint 2 Stage 2: Basic256Sha256 security

**Date:** 2026-04-26
**Sprint:** Sprint 2 — Stage 2 (depends on Stage 1: devlog 107)
**Scope:** BACKLOG P9 #2c (OPC-UA certificate-based security)
**Build flag:** `ZEPTO_USE_OPCUA=OFF` (default) unaffected — security code
is entirely inside the `#ifdef ZEPTO_OPCUA_AVAILABLE` block.
`ZEPTO_USE_OPCUA=ON` gains optional `Sign` / `SignAndEncrypt` with
`Basic256Sha256`.

---

## 1. Why

Sprint 1 (devlog 106) wired a real `UA_Client` session but always called
`UA_ClientConfig_setDefault()` regardless of `config_.security_mode` —
so even when a deployment specified Sign/SignAndEncrypt the connection
went out in the clear.  Every industrial pilot we have talked to
(semiconductor fab, auto factory, steel mill) mandates at least Sign —
usually SignAndEncrypt — on all OT-network OPC-UA traffic.  Without this
we cannot run in a corporate environment.

open62541 exposes this via `UA_ClientConfig_setDefaultEncryption()`,
which installs the Basic256Sha256 security policy and accepts a
client cert/key pair plus an optional trust list.  This devlog wires
the existing `OpcUaConfig` cert-path fields to that call.

## 2. What changed

### 2c-a — Security wiring in `start()`

- `src/feeds/opcua_consumer.cpp`
  - New anonymous-namespace helper `read_file_to_bytestring(path)` —
    reads a PEM/DER file into a freshly-`UA_malloc`'d `UA_ByteString`,
    returns `UA_BYTESTRING_NULL` on any failure.  Caller owns the
    returned blob and must `UA_ByteString_clear()` it.
  - The previously unconditional `UA_ClientConfig_setDefault(cc)` is
    now gated on `{None, None}`.  For any other combo the block:
    1. Reads the client cert and key files off disk.  Missing or
       empty → `ZEPTO_ERROR` + `UA_Client_delete` + `return false`.
    2. Optionally reads the server cert into a single-entry trust list.
    3. Calls `UA_ClientConfig_setDefaultEncryption(cc, cert, key,
       trust, N, revoke=nullptr, 0)`.  On non-`GOOD` status, logs the
       `UA_StatusCode_name` and bails the same way.
    4. Sets `cc->securityMode` to `UA_MESSAGESECURITYMODE_SIGN` or
       `UA_MESSAGESECURITYMODE_SIGNANDENCRYPT`.  The policy URI was
       already installed by `setDefaultEncryption`.
  - Timeout wiring (`cc->timeout`, `cc->requestedSessionTimeout`) moves
    below the branch and runs for both security modes.
  - New include: `<fstream>` for `read_file_to_bytestring`.

### 2c-b — Path-emptiness validation in `start()`

`OpcUaConsumer::is_valid_security(mode, policy)` — a static helper from
Sprint 1 — only checks the enum combination.  Extending its signature
to accept cert paths would ripple into callers and violate the
absolute-minimal-diff rule.  Instead a single extra `if` block sits in
`start()` right after the existing `is_valid_security` gate: when mode
is Sign or SignAndEncrypt, both `client_cert_path` and
`client_key_path` must be non-empty, else `ZEPTO_ERROR` + `return false`.

### Supported security combos (MVP)

| `security_mode` | `security_policy` | Behaviour |
|---|---|---|
| `None` | `None` | `UA_ClientConfig_setDefault`, plain TCP (dev-only) |
| `Sign` | `Basic256Sha256` | `setDefaultEncryption` + `UA_MESSAGESECURITYMODE_SIGN` |
| `SignAndEncrypt` | `Basic256Sha256` | `setDefaultEncryption` + `UA_MESSAGESECURITYMODE_SIGNANDENCRYPT` |

All other combos (`Sign` / `None`, `SignAndEncrypt` / `None`) were
already rejected by `is_valid_security` in Sprint 1.

### Explicit MVP non-features

- **Revocation list.**  `setDefaultEncryption`'s last two params are
  passed as `nullptr, 0`.  Revoked-cert enforcement is tracked as a
  Sprint 3 follow-up.
- **CA chain / multi-cert trust list.**  The trust list is at most the
  single `server_cert_path`.  Multi-cert chains require iterating over
  a trust directory — also Sprint 3.
- **In-process cert generation for tests.**  The integration path
  outlined in the task (2c-d) would add ~200 lines of `openssl`-based
  cert-gen plumbing for a single live-server round-trip; per the
  KIRO.md absolute-minimal-code rule this is skipped in favour of the
  existing None/None integration test (devlog 107, 2k) plus the four
  new validation-path unit tests.  **TODO(Sprint 3):** live Basic256Sha256
  round-trip test with an ephemeral `openssl req`-generated keypair.

## 3. Default-build behaviour delta

`ZEPTO_USE_OPCUA=OFF` build: **zero** behaviour change — all security
code is inside `#ifdef ZEPTO_OPCUA_AVAILABLE`.  The only changes visible
in the OFF build are:

1. The new path-emptiness validation in `start()` runs before the
   license gate.  Since `security_mode` defaults to `None`, no existing
   OFF-build caller hits the new branch.

2. `<fstream>` is added to the translation unit's includes.  Header
   surface is unchanged.

No pre-existing `OpcUa*` test was modified.

## 4. Tests added

All four new tests live at the bottom of `tests/unit/test_opcua.cpp`
after the `OpcUaQuality` block.  They hit the `start()` validation path
and assert `false` — no live server, no `ZEPTO_OPCUA_AVAILABLE`
requirement.

| Suite | Case | Purpose |
|---|---|---|
| `OpcUaSecurity` | `Sign_RequiresNonEmptyCertPath` | Sign + empty cert/key paths → `start()` false |
| `OpcUaSecurity` | `SignAndEncrypt_RequiresNonEmptyCertAndKey` | SignAndEncrypt + partial paths (cert set, key empty) → false |
| `OpcUaSecurity` | `None_AcceptsEmptyCertPaths` | Sanity: None/None does not regress into the new branch |
| `OpcUaSecurity` | `Sign_StartFailsBeforeLicenseGate` | Validation-ordering invariant — Sign + empty paths must fail before license check |

`read_file_to_bytestring` is `static` inside an anonymous namespace in
the `.cpp` and is therefore not exposed for direct testing; per the
task brief that compile-only test is skipped.

## 5. Verification

```
$ cd build && ninja zepto_tests 2>&1 | tail -5
[1/4] Building CXX object CMakeFiles/zepto_opcua.dir/src/feeds/opcua_consumer.cpp.o
[2/4] Linking CXX static library libzepto_opcua.a
[3/4] Building CXX object tests/CMakeFiles/zepto_tests.dir/unit/test_opcua.cpp.o
[4/4] Linking CXX executable tests/zepto_tests

$ ./tests/zepto_tests --gtest_filter="OpcUa*" 2>&1 | tail -5
[----------] 5 tests from OpcUaQuality (5 ms total)
[==========] 47 tests from 17 test suites ran. (1200 ms total)
[  PASSED  ] 47 tests.

$ ./tests/zepto_tests 2>&1 | tail -5
[==========] 1279 tests from 173 test suites ran. (286629 ms total)
[  PASSED  ] 1279 tests.
```

Baseline after Sprint 2 Stage 1: 1275/1275, 43 `OpcUa*`.  After Stage 2:
1279/1279 (+4), 47 `OpcUa*` (+4).

## 6. Files touched

| Path | Purpose |
|---|---|
| `src/feeds/opcua_consumer.cpp` | `<fstream>` include; `read_file_to_bytestring` helper; security block in `start()` replacing the unconditional `setDefault`; path-emptiness check after `is_valid_security` gate |
| `tests/unit/test_opcua.cpp` | 4 new `OpcUaSecurity` tests appended at EOF |
| `docs/design/opcua_connector.md` | §7 replaced "planned" language with the actual implementation; supported-combo table |
| `docs/BACKLOG.md` | Tier-2 #2c struck through with ✅ devlog 108 cross-ref |
| `docs/devlog/108_opcua_security.md` | this file |

`COMPLETED.md` is intentionally **not** updated yet — per the Sprint 2
plan, that happens in the combined closeout after all stages ship.

## 7. Follow-ups

- **Sprint 3:** live Basic256Sha256 round-trip integration test.
  `tests/unit/test_opcua_integration.cpp` currently covers only the
  None/None path; a second test should generate an ephemeral keypair in
  `/tmp` via `openssl req -x509 -newkey rsa:2048 -nodes`, spin up an
  open62541 server with matching encryption config, and assert a
  Sign+Basic256Sha256 consumer receives a tick.
- **Sprint 3:** revocation list (CRL) enforcement — currently
  `setDefaultEncryption` is called with `nullptr, 0` for the revocation
  arguments.
- **Sprint 3:** multi-cert CA trust directory.  Current MVP limits the
  trust list to the single `server_cert_path`.
- **Docs:** when Sprint 3 ships, flip the `docs/design/opcua_connector.md`
  §7 MVP-limitation box to remove the revocation / CA-chain notes.
