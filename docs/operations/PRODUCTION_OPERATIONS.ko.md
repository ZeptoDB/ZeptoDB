# ZeptoDB 프로덕션 운영 가이드

## 목차
- [1. 초기 설정](#1-초기-설정)
- [2. 모니터링](#2-모니터링)
- [3. 로깅](#3-로깅)
- [4. 백업 & 복구](#4-백업--복구)
- [5. 자동화](#5-자동화)
- [6. 문제 해결](#6-문제-해결)

---

## 1. 초기 설정

### 1.1 서비스 설치

```bash
# 1. ZeptoDB 빌드
cd /home/ec2-user/zeptodb
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# 2. 프로덕션 서비스 설치 (root 필요)
cd ..
sudo ./scripts/install_service.sh
```

설치 포함 내용:
- ✅ `zeptodb` 사용자 생성
- ✅ 디렉토리: `/opt/zeptodb`, `/data/zeptodb`, `/var/log/zeptodb`
- ✅ systemd 서비스: `zeptodb.service`
- ✅ Cron 작업: 백업 (02:00), EOD (평일 18:00)
- ✅ 로그 로테이션: 30일 보관

### 1.2 서비스 시작

```bash
# 서비스 시작
sudo systemctl start zeptodb

# 상태 확인
sudo systemctl status zeptodb

# 로그 확인
sudo journalctl -u zeptodb -f
```

### 1.3 헬스 체크

```bash
# Liveness probe (서버 살아있는가?)
curl http://localhost:8123/health
# {"status":"healthy"}

# Readiness probe (쿼리 처리 준비됐는가?)
curl http://localhost:8123/ready
# {"status":"ready"}

# 통계
curl http://localhost:8123/stats
```

---

## 2. 모니터링

### 2.1 Prometheus + Grafana 설정

```bash
# Docker Compose로 모니터링 스택 실행
cd /home/ec2-user/zeptodb/monitoring
docker-compose up -d

# 서비스 확인
docker-compose ps
```

**접속:**
- Grafana: http://localhost:3000 (admin/zepto-admin-2026)
- Prometheus: http://localhost:9090

### 2.2 Grafana 대시보드 설정

1. Grafana 로그인
2. Configuration → Data Sources → Add Prometheus
   - URL: `http://prometheus:9090`
3. Dashboards → Import
   - 파일: `grafana-dashboard.json`

### 2.3 메트릭 확인

```bash
# Prometheus 메트릭 확인
curl http://localhost:8123/metrics
```

**주요 메트릭:**

| 메트릭 | 타입 | 설명 |
|--------|------|-------------|
| `zepto_ticks_ingested_total` | counter | 총 인제스트된 틱 |
| `zepto_ticks_stored_total` | counter | 저장된 틱 |
| `zepto_ticks_dropped_total` | counter | 드롭된 틱 |
| `zepto_queries_executed_total` | counter | 실행된 쿼리 |
| `zepto_rows_scanned_total` | counter | 스캔된 행 |
| `zepto_server_up` | gauge | 서버 실행 상태 (0/1) |
| `zepto_server_ready` | gauge | 준비 상태 (0/1) |

### 2.4 알림 설정

Alertmanager 설정 편집:

```bash
vi /home/ec2-user/zeptodb/monitoring/alertmanager.yml
```

**Slack Webhook 설정:**
1. Slack → Apps → Incoming Webhooks
2. Webhook URL 복사
3. `alertmanager.yml`의 `YOUR_SLACK_WEBHOOK_URL` 교체

**PagerDuty 설정:**
1. PagerDuty → Service → Integrations → Prometheus
2. Integration Key 복사
3. `alertmanager.yml`의 `YOUR_PAGERDUTY_SERVICE_KEY` 교체

---

## 3. 로깅

### 3.1 구조화된 로깅

ZeptoDB는 JSON 형식으로 구조화된 로그를 생성합니다.

**로그 위치:**
- 파일: `/var/log/zeptodb/zeptodb.log`
- systemd: `journalctl -u zeptodb`

**로그 레벨:**
- `TRACE` - 상세 디버그
- `DEBUG` - 개발 정보
- `INFO` - 일반 정보
- `WARN` - 경고
- `ERROR` - 오류
- `CRITICAL` - 치명적 오류

### 3.2 로그 보기

```bash
# 최근 로그 (journalctl)
sudo journalctl -u zeptodb -n 100

# 실시간 로그
sudo journalctl -u zeptodb -f

# JSON 로그 파일
sudo tail -f /var/log/zeptodb/zeptodb.log | jq .

# 오류만 필터링
sudo journalctl -u zeptodb -p err
```

### 3.3 로그 예시

```json
{
  "timestamp": "2026-03-22T14:30:45.123+0900",
  "level": "INFO",
  "message": "Query executed successfully",
  "component": "QueryExecutor",
  "details": "SELECT sum(volume) FROM trades - 1.2ms"
}
```

### 3.4 로그 로테이션

자동 설정됨 (`/etc/logrotate.d/zeptodb`):
- 일별 로테이션
- 30일 보관
- 압축 저장

---

## 4. 백업 & 복구

### 4.1 자동 백업

**Cron 설정 (자동 실행):**
```
0 2 * * * /opt/zeptodb/scripts/backup.sh
```

**수동 백업:**
```bash
sudo -u zeptodb /opt/zeptodb/scripts/backup.sh
```

**백업 내용:**
- HDB (Historical Database)
- WAL (Write-Ahead Log)
- 설정 파일
- 메타데이터

**백업 위치:**
- 로컬: `/backup/zeptodb/zeptodb-backup-YYYYMMDD_HHMMSS.tar.gz`
- S3: `s3://${S3_BUCKET}/backups/` (선택사항)

### 4.2 S3 백업 설정

```bash
# 환경 변수 설정
export ZEPTO_S3_BACKUP_BUCKET="my-apex-backups"
export AWS_REGION="us-east-1"

# 필요한 IAM 권한:
# - s3:PutObject
# - s3:GetObject
# - s3:ListBucket
# - s3:DeleteObject
```

### 4.3 재해 복구

**로컬 백업에서 복구:**
```bash
# 1. ZeptoDB 중지
sudo systemctl stop zeptodb

# 2. 백업 복구
sudo /opt/zeptodb/scripts/restore.sh zeptodb-backup-20260322_020000

# 3. 서비스 재시작
sudo systemctl start zeptodb
```

**S3 백업에서 복구:**
```bash
sudo /opt/zeptodb/scripts/restore.sh zeptodb-backup-20260322_020000 --from-s3
```

**WAL 재생 건너뛰기:**
```bash
sudo /opt/zeptodb/scripts/restore.sh zeptodb-backup-20260322_020000 --skip-wal-replay
```

### 4.4 백업 보관 정책

**로컬:**
- 보관 기간: 30일 (기본)
- `BACKUP_RETENTION_DAYS` 환경 변수로 설정 가능

**S3:**
- Lifecycle 정책 권장
- STANDARD_IA (Infrequent Access) 스토리지 클래스

---

## 5. 자동화

### 5.1 systemd 서비스

**서비스 관리:**
```bash
# 시작
sudo systemctl start zeptodb

# 중지
sudo systemctl stop zeptodb

# 재시작
sudo systemctl restart zeptodb

# 상태
sudo systemctl status zeptodb

# 부팅 시 자동 시작 활성화
sudo systemctl enable zeptodb
```

**서비스 기능:**
- ✅ 자동 재시작 (실패 후 5초)
- ✅ CPU 어피니티 (코어 0-7)
- ✅ OOM 보호 (우선순위 -900)
- ✅ 리소스 제한 (1M 파일, 무제한 메모리)

### 5.2 EOD (End-of-Day) 프로세스

**자동 실행 (cron):**
```
0 18 * * 1-5 /opt/zeptodb/scripts/eod_process.sh
```

**수동 실행:**
```bash
sudo -u zeptodb /opt/zeptodb/scripts/eod_process.sh
```

**EOD 작업:**
1. RDB → HDB 플러시
2. 통계 수집
3. WAL 정리 (7일 후 압축, 30일 후 삭제)
4. 자동 백업
5. 디스크 사용량 확인

**로그:**
```bash
tail -f /var/log/zeptodb/eod.log
```

### 5.3 자동 튜닝

**베어메탈 튜닝:**
```bash
sudo /opt/zeptodb/scripts/tune_bare_metal.sh
```

튜닝 항목:
- CPU 거버너 → performance
- 터보 부스트 활성화
- 휴지페이지 32GB
- IRQ 어피니티
- 네트워크 스택

---

## 6. 문제 해결

### 6.1 서비스가 시작되지 않을 때

```bash
# 로그 확인
sudo journalctl -u zeptodb -n 100

# 설정 파일 확인
cat /opt/zeptodb/config.yaml

# 포트 충돌 확인
sudo lsof -i :8123

# 권한 확인
ls -la /data/zeptodb
```

### 6.2 높은 틱 드롭율

**원인:**
- 링 버퍼 너무 작음
- CPU 과부하
- 디스크 I/O 병목

**해결:**
```bash
# 1. 메트릭 확인
curl http://localhost:8123/metrics | grep dropped

# 2. CPU 사용량 확인
top -u zeptodb

# 3. 링 버퍼 크기 증가 (config.yaml)
ring_buffer_size: 1048576  # 기본 524288

# 4. 재시작
sudo systemctl restart zeptodb
```

### 6.3 느린 쿼리

```bash
# 1. 스캔된 행 확인
curl http://localhost:8123/stats

# 2. 쿼리 실행 계획 확인 (EXPLAIN)
curl -X POST http://localhost:8123/ -d "EXPLAIN SELECT ..."

# 3. HDB 압축 상태 확인
du -sh /data/zeptodb/hdb

# 4. 병렬 쿼리 활성화 확인 (config.yaml)
query_threads: 8
```

### 6.4 디스크 가득 참

```bash
# 1. 사용량 확인
df -h /data/zeptodb

# 2. 오래된 HDB 정리
find /data/zeptodb/hdb -type d -mtime +90 -exec rm -rf {} \;

# 3. WAL 압축
find /data/zeptodb/wal -name "*.wal" -mtime +7 -exec gzip {} \;

# 4. 백업 정리
find /backup/zeptodb -name "*.tar.gz" -mtime +30 -delete
```

### 6.5 메모리 부족

```bash
# 1. 메모리 사용량 확인
free -h
pmap -x $(pgrep zepto-server)

# 2. OOM killer 로그
dmesg | grep -i "out of memory"

# 3. 휴지페이지 확인
cat /proc/meminfo | grep Huge

# 4. 프로세스 재시작 (메모리 해제)
sudo systemctl restart zeptodb
```

### 6.6 Prometheus 메트릭 보이지 않음

```bash
# 1. /metrics 엔드포인트 확인
curl http://localhost:8123/metrics

# 2. Prometheus 타겟 확인
curl http://localhost:9090/api/v1/targets | jq .

# 3. Prometheus 로그
docker logs zepto-prometheus

# 4. 방화벽 확인
sudo iptables -L -n | grep 8123
```

---

## 7. 성능 최적화

### 7.1 베어메탈 환경

```bash
# CPU 격리 (GRUB)
vi /etc/default/grub
# GRUB_CMDLINE_LINUX="isolcpus=0-7 nohz_full=0-7"

# 자동 튜닝
sudo /opt/zeptodb/scripts/tune_bare_metal.sh

# NUMA 토폴로지 확인
numactl --hardware
```

### 7.2 클라우드 환경

**AWS 최적화:**
- 인스턴스: `c7g.16xlarge` (64 vCPU, 128GB RAM)
- 스토리지: `io2` EBS (64K IOPS)
- 네트워크: Enhanced Networking (ENA)
- Placement Group: `cluster`

**Kubernetes 최적화:**
```yaml
# 리소스 할당
resources:
  requests:
    cpu: "32"
    memory: "64Gi"
  limits:
    cpu: "64"
    memory: "128Gi"

# 노드 어피니티
nodeSelector:
  node.kubernetes.io/instance-type: c7g.16xlarge
```

---

## 8. 보안

### 8.1 네트워크 보안

```bash
# 방화벽 설정 (iptables)
sudo iptables -A INPUT -p tcp --dport 8123 -s 10.0.0.0/8 -j ACCEPT
sudo iptables -A INPUT -p tcp --dport 8123 -j DROP

# UFW
sudo ufw allow from 10.0.0.0/8 to any port 8123
```

### 8.2 TLS 설정

```yaml
# config.yaml
server:
  tls:
    enabled: true
    cert_file: /etc/zeptodb/server.crt
    key_file: /etc/zeptodb/server.key
```

### 8.3 인증

```yaml
# config.yaml
auth:
  enabled: true
  type: basic  # or jwt
  users:
    - username: admin
      password_hash: "$2a$10$..."
```

---

## 9. 연락처

**문제 발생 시:**
- GitHub Issues: https://github.com/zeptodb/zeptodb/issues
- Slack: #zeptodb-support
- Email: support@zeptodb.io

**긴급 사건:**
- PagerDuty: ZeptoDB Critical Alerts
- On-call: +1-XXX-XXX-XXXX
