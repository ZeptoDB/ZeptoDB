# Devlog 030 — Login "Invalid Key" Bug Fix (SO_REUSEPORT Port Conflict)

**Date:** 2026-03-25

## Problem

로그인 → 로그아웃 → 재로그인 시 "Invalid API key" 에러 발생.
백엔드 콘솔에서 키를 복사해서 넣어도 동일 증상.

## Root Cause

`httplib`이 `SO_REUSEPORT`를 기본 설정하고 있어서, `zepto_http_server`를 여러 번 실행하면
같은 포트(8123)에 여러 인스턴스가 동시에 바인딩됨.

각 인스턴스는 시작 시점에 `dev_keys.txt`를 로드하고 새 키 3개를 생성하므로,
**나중에 시작된 서버가 생성한 키는 먼저 시작된 서버가 모름.**

커널이 `SO_REUSEPORT`로 요청을 분배하면서:
- 첫 로그인: 서버 A로 라우팅 → 성공
- 재로그인: 서버 B로 라우팅 → 해당 키를 모름 → 401

## Changes

### 1. `tools/zepto_http_server.cpp` — 포트 충돌 감지

시작 전 `connect()`로 포트 사용 여부 체크. 이미 사용 중이면 에러 출력 후 종료.

```cpp
static bool is_port_in_use(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int ret = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    close(fd);
    return ret == 0;
}
```

`bind()` 대신 `connect()`를 사용한 이유: `SO_REUSEPORT`가 설정되어 있으면
`bind()`가 성공해버리므로 충돌을 감지할 수 없음.

### 2. `src/common/logger.cpp` — spdlog 이중 등록 crash 방지

`spdlog::stdout_color_mt("zeptodb")` 호출 전 같은 이름의 로거가 이미 있으면 재사용.

### 3. `src/util/logger.cpp` — register 전 drop

`spdlog::register_logger()` 전에 `spdlog::drop("zeptodb")`로 기존 로거 제거.

### 4. `web/src/lib/auth.tsx` — 프론트엔드 방어 코드

- `fetch`에 `cache: "no-store"` 추가 (브라우저 캐시 방지)
- API 키 `trim()` (복사-붙여넣기 시 공백 제거)
- 백엔드 에러 메시지를 그대로 전달 (디버깅 용이)
- `logout` 시 `cancelQueries()` → `clear()` 순서로 정리

### 5. `web/src/app/login/page.tsx` — 실제 에러 메시지 표시

`catch`에서 고정 문자열 대신 `err.message`를 표시하여 실제 원인 노출.

## Files Modified

- `tools/zepto_http_server.cpp`
- `src/common/logger.cpp`
- `src/util/logger.cpp`
- `web/src/lib/auth.tsx`
- `web/src/app/login/page.tsx`
