# Devlog 030 — Login "Invalid Key" Bug Fix (SO_REUSEPORT Port Conflict)

**Date:** 2026-03-25

## Problem

When logging in → logging out → logging in again, an "Invalid API key" error occurred.
The same symptom appeared even when copying and pasting the key from the backend console.

## Root Cause

`httplib` sets `SO_REUSEPORT` by default, so running `zepto_http_server` multiple times
causes multiple instances to bind to the same port (8123) simultaneously.

Each instance loads `dev_keys.txt` at startup and generates 3 new keys,
so **the server started later has keys that the earlier server does not know about.**

As the kernel distributes requests via `SO_REUSEPORT`:
- First login: routed to Server A → success
- Re-login: routed to Server B → does not recognize the key → 401

## Changes

### 1. `tools/zepto_http_server.cpp` — Port conflict detection

Checks port availability via `connect()` before starting. If already in use, prints an error and exits.

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

Reason for using `connect()` instead of `bind()`: When `SO_REUSEPORT` is set,
`bind()` succeeds anyway, so it cannot detect the conflict.

### 2. `src/common/logger.cpp` — Prevent spdlog duplicate registration crash

Before calling `spdlog::stdout_color_mt("zeptodb")`, reuses the existing logger if one with the same name already exists.

### 3. `src/util/logger.cpp` — Drop before register

Calls `spdlog::drop("zeptodb")` before `spdlog::register_logger()` to remove the existing logger.

### 4. `web/src/lib/auth.tsx` — Frontend defensive code

- Added `cache: "no-store"` to `fetch` (prevents browser caching)
- `trim()` on API key (removes whitespace from copy-paste)
- Passes backend error messages through as-is (easier debugging)
- On `logout`, cleans up in order: `cancelQueries()` → `clear()`

### 5. `web/src/app/login/page.tsx` — Display actual error message

Shows `err.message` in `catch` instead of a hardcoded string to expose the actual cause.

## Files Modified

- `tools/zepto_http_server.cpp`
- `src/common/logger.cpp`
- `src/util/logger.cpp`
- `web/src/lib/auth.tsx`
- `web/src/app/login/page.tsx`
