# 034 — 내부 RPC 보안 (Shared-Secret HMAC + mTLS 준비)

**Date**: 2026-03-27
**Status**: Completed
**P8-Critical**: 내부 RPC 보안

---

## 배경

클러스터 내부 TCP RPC가 평문. 인증/인가 없음.
네트워크 접근 가능한 공격자가 임의 SQL 실행, 틱 주입, WAL 조작 가능.

## 변경 내용

### 신규: `include/zeptodb/cluster/rpc_security.h`

- `RpcSecurityConfig` — `enabled`, `shared_secret`, mTLS 경로 (`cert_path`/`key_path`/`ca_cert_path`)
- `rpc_compute_hmac()` — 4-round FNV-1a 기반 256-bit HMAC (OpenSSL 무의존)
- `rpc_generate_nonce()` — 8-byte random nonce
- `rpc_build_auth()` — nonce(8) + HMAC(32) = 40-byte auth payload 생성
- `rpc_validate_auth()` — 서버 측 HMAC 검증

### 수정: `rpc_protocol.h`
- `AUTH_HANDSHAKE(15)`, `AUTH_OK(16)`, `AUTH_REJECT(17)` 메시지 타입 추가

### 수정: `tcp_rpc.h`
- `TcpRpcServer::set_security()`, `security_` 멤버
- `TcpRpcClient::set_security()`, `security_` 멤버

### 수정: `tcp_rpc.cpp`

**서버 (`handle_connection`)**:
- `security_.enabled` 시 첫 메시지로 AUTH_HANDSHAKE 요구
- HMAC 검증 실패 → AUTH_REJECT + 연결 종료
- 검증 성공 → AUTH_OK 응답 후 정상 메시지 루프 진입

**클라이언트 (`acquire`)**:
- 새 연결 생성 시 `security_.enabled`면 AUTH_HANDSHAKE 전송
- AUTH_OK 수신 확인, 실패 시 연결 닫기
- 풀에서 꺼낸 기존 연결은 이미 인증됨 (재인증 불필요)

## 프로토콜 흐름

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

## 하위 호환성

- `security_.enabled = false` (기본값) → 기존 동작 그대로 (handshake 없음)
- 기존 796개 테스트 전체 통과

## mTLS 향후 계획

`RpcSecurityConfig`에 `cert_path`/`key_path`/`ca_cert_path` 필드가 준비됨.
OpenSSL `SSL_CTX` 래핑은 별도 작업으로 분리 (소켓 전체를 SSL로 감싸야 함).
현재 HMAC 인증은 mTLS 도입 후에도 defense-in-depth 레이어로 유지.
