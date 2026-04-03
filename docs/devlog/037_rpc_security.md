# 034 — Internal RPC Security (Shared-Secret HMAC + mTLS Preparation)

**Date**: 2026-03-27
**Status**: Completed
**P8-Critical**: Internal RPC Security

---

## Background

Cluster-internal TCP RPC is plaintext. No authentication/authorization.
An attacker with network access can execute arbitrary SQL, inject ticks, or manipulate WAL.

## Changes

### New: `include/zeptodb/cluster/rpc_security.h`

- `RpcSecurityConfig` — `enabled`, `shared_secret`, mTLS paths (`cert_path`/`key_path`/`ca_cert_path`)
- `rpc_compute_hmac()` — 4-round FNV-1a based 256-bit HMAC (no OpenSSL dependency)
- `rpc_generate_nonce()` — 8-byte random nonce
- `rpc_build_auth()` — generates nonce(8) + HMAC(32) = 40-byte auth payload
- `rpc_validate_auth()` — server-side HMAC verification

### Modified: `rpc_protocol.h`
- Added `AUTH_HANDSHAKE(15)`, `AUTH_OK(16)`, `AUTH_REJECT(17)` message types

### Modified: `tcp_rpc.h`
- `TcpRpcServer::set_security()`, `security_` member
- `TcpRpcClient::set_security()`, `security_` member

### Modified: `tcp_rpc.cpp`

**Server (`handle_connection`)**:
- When `security_.enabled`, requires AUTH_HANDSHAKE as the first message
- HMAC verification failure → AUTH_REJECT + connection close
- Verification success → AUTH_OK response, then enters normal message loop

**Client (`acquire`)**:
- When creating a new connection, sends AUTH_HANDSHAKE if `security_.enabled`
- Confirms AUTH_OK receipt, closes connection on failure
- Existing connections from the pool are already authenticated (no re-authentication needed)

## Protocol Flow

```
Client                          Server
  │                               │
  ├─ connect() ──────────────────→│
  ├─ AUTH_HANDSHAKE{nonce+HMAC} ─→│
  │                               ├─ validate HMAC
  │                               │   ├─ OK → AUTH_OK
  │←─ AUTH_OK ────────────────────┤   └─ FAIL → AUTH_REJECT + close
  │                               │
  ├─ SQL_QUERY / TICK_INGEST ────→│  (normal messages)
  │←─ SQL_RESULT / TICK_ACK ──────┤
```

## Backward Compatibility

- `security_.enabled = false` (default) → existing behavior unchanged (no handshake)
- All existing 796 tests passed

## mTLS Future Plans

`RpcSecurityConfig` has `cert_path`/`key_path`/`ca_cert_path` fields prepared.
OpenSSL `SSL_CTX` wrapping is separated into a separate task (requires wrapping entire socket with SSL).
The current HMAC authentication will be retained as a defense-in-depth layer even after mTLS is introduced.
