# APEX-DB × Physical AI — 비즈니스 기회 분석
# 2026-03-22

---

## 1. 시장 개요

Physical AI(물리 세계와 상호작용하는 AI 시스템)는 2025~2030년 가장 빠르게 성장하는 기술 분야다.
핵심 특성이 HFT와 구조적으로 동일하다: **고빈도 시계열 데이터 + 실시간 의사결정 + μs 레이턴시**.

### 타겟 시장 규모

| 분야 | 2025 시장 | 2030 예상 | CAGR |
|---|---|---|---|
| 자율주행 데이터 인프라 | $8B | $35B | ~34% |
| 산업용 IoT / 스마트팩토리 | $110B | $280B | ~20% |
| 로봇 소프트웨어 인프라 | $12B | $60B | ~38% |
| 드론 관제 / 군집 | $3B | $18B | ~43% |

---

## 2. 분야별 비즈니스 케이스

---

### A. 자율주행 (Autonomous Vehicles)

**고객:** Waymo, Cruise, 현대 모비스, NVIDIA DRIVE 플랫폼 파트너, 중국 OEM

**데이터 특성:**
```
LiDAR:    10Hz × 100,000 포인트/scan = 1M 포인트/sec
Camera:   30fps × 8대 = 240 프레임/sec
IMU/GPS:  1,000Hz
Radar:    100Hz
CAN bus:  1,000+ 채널 × 100Hz
→ 단일 차량: 초당 수백만 타임스탬프 데이터포인트
```

**현재 고통:**
- ROS bag 파일로 수집 → 오프라인 분석만 가능
- 실시간 센서 융합(Sensor Fusion)에 Redis 사용 → 레이턴시 불안정
- Python 기반 분석 파이프라인이 C++ 실시간 루프와 분리됨

**APEX-DB 해결책:**
```sql
-- 카메라-LiDAR 시간 동기화 (ASOF JOIN)
SELECT c.frame_id, c.timestamp, l.scan_data
FROM camera_frames c
ASOF JOIN lidar_scans l
ON c.sensor_id = l.sensor_id AND c.timestamp >= l.timestamp

-- 전후 50ms 센서 통합 (Window JOIN)
SELECT imu.accel_x, wj_avg(gps.latitude) AS smoothed_lat
FROM imu_data imu
WINDOW JOIN gps_data gps
ON imu.vehicle_id = gps.vehicle_id
AND gps.timestamp BETWEEN imu.timestamp - 50000000 AND imu.timestamp + 50000000

-- 실시간 이상 감지
SELECT timestamp, sensor_id, DELTA(vibration) AS vibration_delta
FROM vehicle_sensors
WHERE DELTA(vibration) OVER (ORDER BY timestamp) > 500
```

**영업 메시지:**
> "ROS bag + 오프라인 분석 시대는 끝났습니다. APEX-DB로 차량 센서 데이터를 실시간으로 융합하고, Python ML 파이프라인에 zero-copy로 연결하세요. 522ns latency."

**경쟁 대비 우위:**
- InfluxDB: ms 레이턴시, ASOF JOIN 없음
- TimescaleDB: SQL 있지만 SIMD 없음, 느림
- Redis: 영구 저장 없음, 집계 불가
- **APEX-DB: μs 레이턴시 + ASOF/Window JOIN + Python zero-copy + HDB 히스토리**

---

### B. 스마트팩토리 / 산업 IoT

**고객:** 삼성전자, SK하이닉스, TSMC, Siemens, Bosch, 현대제철, POSCO

**데이터 특성:**
```
반도체 팹: CMP 압력 센서 10KHz × 500대 = 5M 포인트/sec
자동차 공장: 용접 로봇 전류/전압 1KHz × 200대 = 200K 포인트/sec
철강: 압연 롤 진동 5KHz × 50개 = 250K 포인트/sec
```

**현재 고통:**
- InfluxDB/Prometheus로는 10KHz 이상 고빈도 데이터 처리 불가
- 이상 감지가 배치 처리 → 사고 후 분석
- 설비 예지보전에 ML 모델 연결이 복잡

**APEX-DB 해결책:**
```sql
-- 실시간 예지보전 (이상 감지)
SELECT equipment_id, timestamp,
       EMA(vibration, 0.1) OVER (PARTITION BY equipment_id) AS ema_slow,
       EMA(vibration, 20) OVER (PARTITION BY equipment_id) AS ema_fast,
       DELTA(temperature) OVER (PARTITION BY equipment_id) AS temp_rate
FROM factory_sensors
WHERE DELTA(vibration) OVER (...) > threshold

-- 생산라인 xbar 집계 (1분봉 KPI)
SELECT xbar(timestamp, 60000000000) AS minute,
       avg(yield_rate) AS yield, count(*) AS samples
FROM production_line
GROUP BY xbar(timestamp, 60000000000)
```

**비즈니스 가치:**
- 반도체 팹 1분 다운타임 = $500K 손실
- 실시간 이상 감지로 10분 → 10초 감지 시간 단축
- 예지보전으로 계획 외 다운타임 80% 감소

**영업 메시지:**
> "반도체 팹의 10KHz 센서 데이터를 실시간으로 처리하는 DB는 없었습니다. APEX-DB는 5.52M 포인트/sec 인제스션, 272μs 이상 감지, Python ML 직결로 예지보전을 현실로 만듭니다."

---

### C. 로봇 (Humanoid / Industrial Robot)

**고객:** Figure AI, Boston Dynamics, FANUC, ABB, 현대로보틱스, NVIDIA Isaac 파트너

**데이터 특성:**
```
휴머노이드 로봇:
  관절 토크: 30개 관절 × 1KHz = 30K 포인트/sec
  촉각 센서: 손가락 × 100Hz
  비전: 2개 카메라 × 30fps
  → 실시간 정책 업데이트 (강화학습)

산업용 로봇:
  모터 전류: 6축 × 1KHz
  힘/토크: 6DOF × 500Hz
```

**Physical AI 특수 요구사항:**
```
로봇 학습 루프:
센서 → 실시간 Feature 계산 → 신경망 추론 → 행동 → 다시 센서
                    ↑
            여기에 APEX-DB 필요
            (Feature Store + Replay Buffer)
```

**APEX-DB 해결책:**
```python
import apex

# 로봇 센서 스트림 → Feature Store
db = apex.Pipeline()
db.start()

# 실시간 feature 계산 (C++ 엔진)
joint_features = db.query("""
    SELECT joint_id,
           EMA(torque, 10) OVER (PARTITION BY joint_id) AS ema_torque,
           DELTA(position) OVER (PARTITION BY joint_id) AS velocity,
           AVG(torque) OVER (PARTITION BY joint_id ROWS 100 PRECEDING) AS ma100
    FROM robot_joints
    ORDER BY timestamp DESC LIMIT 30
""")

# zero-copy → PyTorch tensor (522ns)
torque_array = db.get_column(joint_id=0, name="ema_torque")
tensor = torch.from_numpy(torque_array)  # 복사 없음
action = policy_network(tensor)
```

**영업 메시지:**
> "로봇 강화학습의 병목은 Feature Store입니다. APEX-DB의 Python zero-copy로 센서 → Feature → PyTorch 파이프라인을 522ns에 연결하세요. NVIDIA Isaac 호환."

---

### D. 드론 군집 (Drone Swarm)

**고객:** Zipline, Wing (Google), 방산업체, 농업드론 기업, 물류 자동화

**데이터 특성:**
```
드론 100대 군집:
  GPS/IMU: 100Hz × 100대 = 10K 포인트/sec
  배터리/모터: 50Hz × 100대 = 5K 포인트/sec
  충돌 회피: 각 드론이 주변 50m 내 드론 위치 실시간 파악
```

**APEX-DB 분산 클러스터 활용:**
```
Ground Station (코디네이터 노드)
    ├── Edge Node 1 (드론 1~25 담당)
    ├── Edge Node 2 (드론 26~50 담당)
    ├── Edge Node 3 (드론 51~75 담당)
    └── Edge Node 4 (드론 76~100 담당)

각 엣지 노드: APEX-DB LocalQueryScheduler
노드 간: UCX Transport (향후 CXL)
```

```sql
-- 충돌 회피: 내 주변 50m 드론 (Window JOIN)
SELECT d1.drone_id, wj_avg(d2.latitude) AS nearby_lat
FROM drone_positions d1
WINDOW JOIN drone_positions d2
ON d2.timestamp BETWEEN d1.timestamp - 100000000 AND d1.timestamp + 100000000
WHERE haversine(d1.lat, d1.lon, d2.lat, d2.lon) < 50
```

---

## 3. APEX-DB × Physical AI 포지셔닝 매트릭스

```
                    LOW LATENCY
                        ↑
           APEX-DB ●    │
            (μs)         │
                         │    ● Redis
                         │      (ms, no persistence)
 FINANCIAL ──────────────┼────────────── PHYSICAL AI
   ONLY                  │                 BOTH
                         │
              ● InfluxDB │ ● TimescaleDB
                (ms)     │   (ms, SQL)
                         │
                    HIGH LATENCY
```

APEX-DB만이 **금융 + Physical AI 양쪽**에서 경쟁력 있는 유일한 오픈소스 DB.

---

## 4. Go-To-Market 전략

### Phase 1: 금융 (현재)
- kdb+ 대체 포지셔닝
- HFT / 퀀트 헤지펀드 타겟
- 싱가포르 금융 허브 거점

### Phase 2: 스마트팩토리 (6~12개월)
- 반도체 팹 (삼성, SK, TSMC) — 한국/대만 네트워크 활용
- InfluxDB/Prometheus 대체 포지셔닝
- "10KHz 센서 데이터를 처리하는 유일한 오픈소스 DB"

### Phase 3: 자율주행 / 로봇 (12~24개월)
- NVIDIA Isaac 에코시스템 연계
- ROS2 플러그인 개발
- "Sensor Fusion DB" 포지셔닝

### Phase 4: 드론 / 엣지 (24개월+)
- 분산 엣지 클러스터 완성 후
- 방산 / 물류 자동화

---

## 5. 웹사이트 반영 제안

현재 타겟 마켓 4개 → 7개로 확장:

기존:
- HFT / 헤지펀드
- 퀀트 리서치
- 리스크/컴플라이언스
- 암호화폐 거래소

추가:
- **🤖 Robotics & Physical AI** — Feature Store, 실시간 센서 처리
- **🏭 Smart Factory** — 예지보전, 실시간 이상 감지
- **🚗 Autonomous Vehicles** — Sensor Fusion, 멀티센서 ASOF JOIN

---

## 6. 핵심 메시지

> **"HFT에서 증명된 기술, Physical AI로 확장."**
>
> μs 레이턴시는 고빈도 거래에만 필요한 게 아닙니다.
> 로봇이 장애물을 피하고, 자율주행차가 센서를 융합하고,
> 공장이 고장을 예측하는 데도 필요합니다.
>
> APEX-DB: 시계열 데이터가 생명인 모든 곳.
