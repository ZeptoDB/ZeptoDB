#!/bin/bash
# ============================================================================
# ZeptoDB 재해 복구 스크립트
# ============================================================================
# 용도: 백업으로부터 데이터 복원 + WAL replay
# 실행: ./restore.sh <backup-name> [--from-s3]
# ============================================================================

set -euo pipefail

# ============================================================================
# 설정
# ============================================================================
APEX_HOME="${APEX_HOME:-/opt/zeptodb}"
DATA_DIR="${APEX_DATA_DIR:-/data/zeptodb}"
BACKUP_DIR="${APEX_BACKUP_DIR:-/backup/zeptodb}"
S3_BUCKET="${ZEPTO_S3_BACKUP_BUCKET:-}"
S3_REGION="${AWS_REGION:-us-east-1}"

LOG_FILE="${BACKUP_DIR}/restore.log"

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
# 사용법
# ============================================================================
usage() {
    cat <<EOF
Usage: $0 <backup-name> [OPTIONS]

Options:
  --from-s3         백업을 S3에서 다운로드
  --skip-wal-replay WAL replay 건너뛰기
  --help            도움말 표시

Examples:
  # 로컬 백업 복원
  $0 zeptodb-backup-20260322_020000

  # S3 백업 복원
  $0 zeptodb-backup-20260322_020000 --from-s3

  # WAL replay 없이 복원
  $0 zeptodb-backup-20260322_020000 --skip-wal-replay
EOF
    exit 1
}

# ============================================================================
# 인자 파싱
# ============================================================================
if [ $# -lt 1 ]; then
    usage
fi

BACKUP_NAME="$1"
FROM_S3=false
SKIP_WAL_REPLAY=false

shift
while [ $# -gt 0 ]; do
    case "$1" in
        --from-s3)
            FROM_S3=true
            ;;
        --skip-wal-replay)
            SKIP_WAL_REPLAY=true
            ;;
        --help)
            usage
            ;;
        *)
            log_error "Unknown option: $1"
            usage
            ;;
    esac
    shift
done

# ============================================================================
# ZeptoDB 중지 확인
# ============================================================================
log "Checking if ZeptoDB is stopped..."
if systemctl is-active --quiet zeptodb; then
    log_error "ZeptoDB is still running. Stop it first:"
    log_error "  sudo systemctl stop zeptodb"
    exit 1
fi

# ============================================================================
# 1. S3에서 백업 다운로드 (옵션)
# ============================================================================
BACKUP_ARCHIVE="${BACKUP_DIR}/${BACKUP_NAME}.tar.gz"

if [ "$FROM_S3" = true ]; then
    log "Downloading backup from S3..."

    if [ -z "$S3_BUCKET" ]; then
        log_error "S3_BUCKET not set"
        exit 1
    fi

    if ! command -v aws &> /dev/null; then
        log_error "AWS CLI not found"
        exit 1
    fi

    aws s3 cp "s3://${S3_BUCKET}/backups/${BACKUP_NAME}.tar.gz" \
        "$BACKUP_ARCHIVE" \
        --region "$S3_REGION"

    log "Download completed"
fi

# ============================================================================
# 2. 백업 파일 확인
# ============================================================================
if [ ! -f "$BACKUP_ARCHIVE" ]; then
    log_error "Backup file not found: $BACKUP_ARCHIVE"
    exit 1
fi

log "Found backup: $BACKUP_ARCHIVE"

# ============================================================================
# 3. 기존 데이터 백업 (안전장치)
# ============================================================================
if [ -d "$DATA_DIR" ]; then
    OLD_BACKUP="${DATA_DIR}.pre-restore.$(date +%Y%m%d_%H%M%S)"
    log "Backing up existing data to: $OLD_BACKUP"
    mv "$DATA_DIR" "$OLD_BACKUP"
fi

# ============================================================================
# 4. 백업 압축 해제
# ============================================================================
log "Extracting backup..."
mkdir -p "$DATA_DIR"
cd "$BACKUP_DIR"
tar -xzf "${BACKUP_NAME}.tar.gz"

# ============================================================================
# 5. HDB 복원
# ============================================================================
if [ -d "${BACKUP_DIR}/${BACKUP_NAME}/hdb" ]; then
    log "Restoring HDB..."
    rsync -a --quiet "${BACKUP_DIR}/${BACKUP_NAME}/hdb/" "${DATA_DIR}/hdb/"
    log "HDB restored"
else
    log "No HDB in backup"
fi

# ============================================================================
# 6. WAL 복원
# ============================================================================
if [ -d "${BACKUP_DIR}/${BACKUP_NAME}/wal" ]; then
    log "Restoring WAL..."
    rsync -a --quiet "${BACKUP_DIR}/${BACKUP_NAME}/wal/" "${DATA_DIR}/wal/"
    log "WAL restored"
else
    log "No WAL in backup"
fi

# ============================================================================
# 7. Config 복원
# ============================================================================
if [ -f "${BACKUP_DIR}/${BACKUP_NAME}/config.yaml" ]; then
    log "Restoring config..."
    cp "${BACKUP_DIR}/${BACKUP_NAME}/config.yaml" "${APEX_HOME}/config.yaml"
    log "Config restored"
fi

# ============================================================================
# 8. 압축 해제된 백업 디렉토리 삭제
# ============================================================================
rm -rf "${BACKUP_DIR}/${BACKUP_NAME}"

# ============================================================================
# 9. WAL Replay (옵션)
# ============================================================================
if [ "$SKIP_WAL_REPLAY" = false ] && [ -d "${DATA_DIR}/wal" ]; then
    log "Replaying WAL..."

    # WAL replay 바이너리 실행 (ZeptoDB에 구현 필요)
    if [ -f "${APEX_HOME}/bin/zepto-wal-replay" ]; then
        "${APEX_HOME}/bin/zepto-wal-replay" --data-dir "$DATA_DIR"
        log "WAL replay completed"
    else
        log "WAL replay binary not found, skipping"
    fi
else
    log "Skipping WAL replay"
fi

# ============================================================================
# 10. 권한 설정
# ============================================================================
log "Setting permissions..."
chown -R zeptodb:zeptodb "$DATA_DIR" 2>/dev/null || true
chmod -R 750 "$DATA_DIR"

# ============================================================================
# 완료
# ============================================================================
log "Restore completed successfully!"
log "You can now start ZeptoDB:"
log "  sudo systemctl start zeptodb"
exit 0
