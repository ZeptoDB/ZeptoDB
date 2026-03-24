#!/bin/bash
# ============================================================================
# ZeptoDB EOD (End-of-Day) 프로세스
# ============================================================================
# 용도: 장 마감 후 RDB → HDB 플러시, 백업, 정리
# 실행: cron으로 장 마감 후 자동 실행
# 예: 0 18 * * 1-5 /opt/zeptodb/scripts/eod_process.sh
# ============================================================================

set -euo pipefail

# ============================================================================
# 설정
# ============================================================================
APEX_HOME="${APEX_HOME:-/opt/zeptodb}"
DATA_DIR="${APEX_DATA_DIR:-/data/zeptodb}"
LOG_FILE="${ZEPTO_LOG_DIR:-/var/log/zeptodb}/eod.log"
BACKUP_SCRIPT="${APEX_HOME}/scripts/backup.sh"

# HTTP API
APEX_HOST="${APEX_HOST:-localhost}"
APEX_PORT="${APEX_PORT:-8123}"

# ============================================================================
# 로그 함수
# ============================================================================
log() {
    echo "[$(date +'%Y-%m-%d %H:%M:%S')] $*" | tee -a "$LOG_FILE"
}

log_error() {
    echo "[$(date +'%Y-%m-%d %H:%M:%S')] ERROR: $*" | tee -a "$LOG_FILE" >&2
}

# ============================================================================
# 헬스체크
# ============================================================================
log "Starting EOD process..."

if ! curl -s "http://${APEX_HOST}:${APEX_PORT}/ready" | grep -q "ready"; then
    log_error "ZeptoDB is not ready"
    exit 1
fi

log "ZeptoDB is ready"

# ============================================================================
# 1. RDB → HDB 플러시
# ============================================================================
log "Flushing RDB to HDB..."

# HTTP API로 플러시 명령 전송 (SQL)
FLUSH_SQL="FLUSH TO HDB"
RESPONSE=$(curl -s -X POST "http://${APEX_HOST}:${APEX_PORT}/" \
    -d "$FLUSH_SQL" \
    -H "Content-Type: text/plain")

if echo "$RESPONSE" | grep -q "error"; then
    log_error "Flush failed: $RESPONSE"
    exit 1
fi

log "RDB flushed to HDB successfully"

# ============================================================================
# 2. 통계 수집
# ============================================================================
log "Collecting statistics..."

STATS=$(curl -s "http://${APEX_HOST}:${APEX_PORT}/stats")
log "Statistics: $STATS"

# ============================================================================
# 3. WAL 정리 (오래된 파일 압축)
# ============================================================================
log "Cleaning up WAL files..."

if [ -d "${DATA_DIR}/wal" ]; then
    # 7일 이상 된 WAL 파일 압축
    find "${DATA_DIR}/wal" -name "*.wal" -type f -mtime +7 -exec gzip {} \;

    # 30일 이상 된 압축 파일 삭제
    find "${DATA_DIR}/wal" -name "*.wal.gz" -type f -mtime +30 -delete

    WAL_COUNT=$(find "${DATA_DIR}/wal" -name "*.wal" | wc -l)
    log "WAL cleanup completed: ${WAL_COUNT} active files"
fi

# ============================================================================
# 4. 백업 실행
# ============================================================================
if [ -f "$BACKUP_SCRIPT" ]; then
    log "Running backup..."
    bash "$BACKUP_SCRIPT"
    log "Backup completed"
else
    log "Backup script not found, skipping"
fi

# ============================================================================
# 5. 디스크 사용량 체크
# ============================================================================
log "Checking disk usage..."

DATA_USAGE=$(df -h "$DATA_DIR" | tail -1 | awk '{print $5}' | sed 's/%//')
if [ "$DATA_USAGE" -gt 80 ]; then
    log_error "Disk usage is high: ${DATA_USAGE}%"
    # 알림 전송 (Slack, PagerDuty 등)
    # curl -X POST https://hooks.slack.com/... -d "{\"text\":\"ZeptoDB disk usage: ${DATA_USAGE}%\"}"
fi

log "Disk usage: ${DATA_USAGE}%"

# ============================================================================
# 6. 메모리 정리 (옵션 - 프로세스 재시작)
# ============================================================================
# 메모리 단편화 방지를 위해 주기적 재시작 (옵션)
# RESTART_INTERVAL_DAYS=7
# LAST_RESTART=$(systemctl show zeptodb --property=ActiveEnterTimestamp | cut -d= -f2)
# DAYS_RUNNING=$(( ($(date +%s) - $(date -d "$LAST_RESTART" +%s)) / 86400 ))
#
# if [ $DAYS_RUNNING -ge $RESTART_INTERVAL_DAYS ]; then
#     log "Restarting ZeptoDB (${DAYS_RUNNING} days running)..."
#     systemctl restart zeptodb
#     sleep 10
#     log "ZeptoDB restarted"
# fi

# ============================================================================
# 완료
# ============================================================================
log "EOD process completed successfully"
exit 0
