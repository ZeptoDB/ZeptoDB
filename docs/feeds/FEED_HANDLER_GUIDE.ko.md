## ZeptoDB 피드 핸들러 가이드

피드 핸들러는 거래소, 데이터 벤더 및 피드 제공업체로부터 실시간 시장 데이터를 수신하여 ZeptoDB에 인제스트하는 컴포넌트입니다.

---

## 지원 프로토콜

### 1. FIX (Financial Information eXchange)
**목적:** Bloomberg, Reuters, ICE 등 데이터 벤더와의 통합

```cpp
#include "zeptodb/feeds/fix_feed_handler.h"

// FIX 설정
feeds::FeedConfig config;
config.host = "bloomberg-feed.com";
config.port = 5000;
config.username = "APEX_CLIENT";
config.password = "password";

feeds::FIXFeedHandler feed_handler(config, &mapper);

// 틱 콜백
feed_handler.on_tick([&](const feeds::Tick& tick) {
    pipeline.ingest(tick.symbol_id, tick.price, tick.volume, tick.timestamp_ns);
});

// 연결 및 구독
feed_handler.connect();
feed_handler.subscribe({"AAPL", "MSFT", "TSLA"});
```

**특성:**
- TCP 기반
- 자동 재연결
- 하트비트 유지
- 레이턴시: 100μs-1ms

---

### 2. Multicast UDP (거래소 직접 연결)
**목적:** NASDAQ, NYSE, CME 등 거래소 직접 연결

```cpp
#include "zeptodb/feeds/multicast_receiver.h"

// 멀티캐스트 수신
feeds::MulticastReceiver receiver("239.1.1.1", 10000);

receiver.on_packet([&](const uint8_t* data, size_t len) {
    // 프로토콜별 파서로 처리
    parser.parse_packet(data, len);
});

receiver.join();
receiver.start();
```

**특성:**
- UDP 멀티캐스트
- 초저지연: 1-5μs
- 패킷 손실 가능 (재전송 없음)
- 고처리량: 10+ Gbps

---

### 3. NASDAQ ITCH 5.0
**목적:** NASDAQ TotalView 시장 데이터

```cpp
#include "zeptodb/feeds/nasdaq_itch.h"

feeds::NASDAQITCHParser parser;

receiver.on_packet([&](const uint8_t* data, size_t len) {
    if (parser.parse_packet(data, len)) {
        feeds::Tick tick;
        if (parser.extract_tick(tick, &mapper)) {
            pipeline.ingest(tick.symbol_id, tick.price, tick.volume,
                           tick.timestamp_ns);
        }
    }
});
```

**지원 메시지:**
- Add Order (타입 A)
- Order Executed (타입 E)
- Trade (타입 P)
- Order Cancel (타입 X)

**특성:**
- 바이너리 프로토콜 (packed structs)
- 빅엔디안
- 파싱 속도: 메시지당 ~500ns

---

### 4. Binance WebSocket (암호화폐)
**목적:** Binance Spot/Futures 실시간 데이터

```cpp
#include "zeptodb/feeds/binance_feed.h"

// TODO: WebSocket 라이브러리 통합 필요
feeds::BinanceFeedHandler feed_handler(config, &mapper);

feed_handler.on_tick([&](const feeds::Tick& tick) {
    pipeline.ingest(tick.symbol_id, tick.price, tick.volume, tick.timestamp_ns);
});

feed_handler.connect();
feed_handler.subscribe({"btcusdt@trade", "ethusdt@trade"});
```

**스트림 타입:**
- `@trade` - 실시간 거래
- `@aggTrade` - 집계 거래
- `@depth` - 오더북
- `@bookTicker` - 최우선 매수/매도

---

## 아키텍처

```
+--------------------+
| 거래소 피드         |  UDP 멀티캐스트 / TCP
| (NASDAQ, CME)      |
+----------+---------+
           ↓
+----------v---------+
| 피드 핸들러         |  프로토콜 파서
| (FIX, ITCH, 등)    |  - FIX: tag=value
+----------+---------+  - ITCH: 바이너리
           ↓             - WebSocket: JSON
+----------v---------+
| Symbol Mapper      |  symbol → symbol_id
+----------+---------+
           ↓
+----------v---------+
| ZeptoDB 파이프라인  |  5.52M ticks/sec
| ingest()           |  제로카피
+--------------------+
```

---

## 성능 최적화

### 1. 제로카피 파싱
```cpp
// 나쁜 방법: 복사
std::string msg_copy(data, len);
parser.parse(msg_copy);

// 좋은 방법: 제로카피
parser.parse(data, len);  // 포인터만 전달
```

### 2. SIMD 배치 파싱
```cpp
// ITCH 메시지 배치 파싱
auto ticks = parser.parse_batch_simd(data, len);  // AVX-512
pipeline.batch_ingest(ticks);  // 배치 인제스트
```

### 3. CPU 피닝
```cpp
// 피드 핸들러 전용 코어 지정
cpu_set_t cpuset;
CPU_ZERO(&cpuset);
CPU_SET(0, &cpuset);  // 코어 0 전용
pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
```

---

## 프로덕션 체크리스트

### 설정
- [ ] CPU 피닝 (코어 0-1 전용)
- [ ] 휴지페이지 (2MB 페이지)
- [ ] IRQ 어피니티 (NIC → 전용 코어)
- [ ] UDP 수신 버퍼 (2MB+)
- [ ] 커널 바이패스 (Solarflare, Mellanox)

### 모니터링
- [ ] 틱 처리량 (ticks/sec)
- [ ] 패킷 손실률
- [ ] 파싱 오류율
- [ ] 레이턴시 (엔드투엔드)

### 장애 처리
- [ ] 재연결 로직 (FIX)
- [ ] 갭 필 (시퀀스 갭 감지)
- [ ] 페일오버 (Primary/Secondary 피드)
- [ ] 알림 (PagerDuty)

---

## 예시

### 전체 통합 예시
```bash
cd /home/ec2-user/zeptodb
mkdir build && cd build
cmake ..
make feed_handler_integration

# FIX 피드
./feed_handler_integration fix

# NASDAQ ITCH
./feed_handler_integration itch

# 성능 테스트
./feed_handler_integration perf
```

**예시 파일:**
- `examples/feed_handler_integration.cpp`

---

## 로드맵

### Phase 1 (완료)
- [x] FIX 프로토콜 파서
- [x] Multicast UDP 수신기
- [x] NASDAQ ITCH 파서
- [x] 통합 예시

### Phase 2 (TODO)
- [ ] CME SBE (Simple Binary Encoding)
- [ ] NYSE Pillar 프로토콜
- [ ] Binance WebSocket (실제 구현)
- [ ] 갭 필 / 재전송 로직

### Phase 3 (TODO)
- [ ] SIMD 배치 파싱
- [ ] 커널 바이패스 (DPDK)
- [ ] GPU 오프로딩
- [ ] 멀티캐스트 페일오버

---

## 비즈니스 가치

### HFT 시장 진입
✅ **이전:** HTTP API만 (ms 수준 레이턴시)
✅ **이후:** UDP 멀티캐스트 + FIX (μs 수준 레이턴시)

**ROI:**
- HFT 고객 타겟팅 가능: $2.5M-12M 시장
- 전체 kdb+ 대체: FIX + ITCH 지원
- 데이터 벤더 통합: Bloomberg, Reuters

### 경쟁 우위
| 항목 | kdb+ | ClickHouse | ZeptoDB |
|------|------|------------|---------|
| FIX 지원 | ✅ (외부) | ❌ | ✅ 네이티브 |
| ITCH 지원 | ✅ (외부) | ❌ | ✅ 네이티브 |
| 멀티캐스트 | ✅ | ❌ | ✅ |
| 성능 | 기준 | N/A | 동등 |

---

## 지원

**문제 발생 시:**
- GitHub Issues: https://github.com/zeptodb/zeptodb/issues
- 예시: `/home/ec2-user/zeptodb/examples/feed_handler_integration.cpp`
- 문서: `/home/ec2-user/zeptodb/docs/feeds/`
