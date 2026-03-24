#!/bin/bash
# ============================================================================
# ZeptoDB 자동 백업 스크립트
# ============================================================================
# 용도: HDB/WAL/Config 백업 및 S3 업로드 (선택)
# 실행: cron으로 일별 자동 실행
# 예: 0 2 * * * /opt/zeptodb/scripts/backup.sh
# ============================================================================

set -euo pipefail

# ============================================================================
# 설정
# ============================================================================
APEX_HOME="${APEX_HOME:-/opt/zeptodb}"
DATA_DIR="${APEX_DATA_DIR:-/data/zeptodb}"
BACKUP_DIR="${APEX_BACKUP_DIR:-/backup/zeptodb}"
RETENTION_DAYS="${BACKUP_RETENTION_DAYS:-30}"
S3_BUCKET="${ZEPTO_S3_BACKUP_BUCKET:-}"
S3_REGION="${AWS_REGION:-us-east-1}"

# 타임스탬프
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
BACKUP_NAME="zeptodb-backup-${TIMESTAMP}"
BACKUP_PATH="${BACKUP_DIR}/${BACKUP_NAME}"

# 로그
LOG_FILE="${BACKUP_DIR}/backup.log"

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
# 백업 디렉토리 생성
# ============================================================================
log "Starting backup: ${BACKUP_NAME}"
mkdir -p "$BACKUP_DIR"
mkdir -p "$BACKUP_PATH"

# ============================================================================
# 1. HDB 백업 (Historical Database)
# ============================================================================
if [ -d "${DATA_DIR}/hdb" ]; then
    log "Backing up HDB..."
    rsync -a --quiet "${DATA_DIR}/hdb/" "${BACKUP_PATH}/hdb/"
    HDB_SIZE=$(du -sh "${BACKUP_PATH}/hdb" | cut -f1)
    log "HDB backup completed: ${HDB_SIZE}"
else
    log "No HDB directory found, skipping"
fi

# ============================================================================
# 2. WAL 백업 (Write-Ahead Log)
# ============================================================================
if [ -d "${DATA_DIR}/wal" ]; then
    log "Backing up WAL..."
    rsync -a --quiet "${DATA_DIR}/wal/" "${BACKUP_PATH}/wal/"
    WAL_SIZE=$(du -sh "${BACKUP_PATH}/wal" | cut -f1)
    log "WAL backup completed: ${WAL_SIZE}"
else
    log "No WAL directory found, skipping"
fi

# ============================================================================
# 3. Config 백업
# ============================================================================
if [ -f "${APEX_HOME}/config.yaml" ]; then
    log "Backing up config..."
    cp "${APEX_HOME}/config.yaml" "${BACKUP_PATH}/config.yaml"
    log "Config backup completed"
fi

# ============================================================================
# 4. 메타데이터 생성
# ============================================================================
cat > "${BACKUP_PATH}/metadata.json" <<EOF
{
  "backup_name": "${BACKUP_NAME}",
  "timestamp": "${TIMESTAMP}",
  "hostname": "$(hostname)",
  "data_dir": "${DATA_DIR}",
  "hdb_size": "${HDB_SIZE:-0}",
  "wal_size": "${WAL_SIZE:-0}"
}
EOF

# ============================================================================
# 5. 압축 (gzip)
# ============================================================================
log "Compressing backup..."
cd "$BACKUP_DIR"
tar -czf "${BACKUP_NAME}.tar.gz" "${BACKUP_NAME}"
COMPRESSED_SIZE=$(du -sh "${BACKUP_NAME}.tar.gz" | cut -f1)
log "Compression completed: ${COMPRESSED_SIZE}"

# 압축 후 원본 디렉토리 삭제
rm -rf "$BACKUP_PATH"

# ============================================================================
# 6. S3 업로드 (옵션)
# ============================================================================
if [ -n "$S3_BUCKET" ]; then
    log "Uploading to S3: s3://${S3_BUCKET}/backups/${BACKUP_NAME}.tar.gz"

    if command -v aws &> /dev/null; then
        aws s3 cp "${BACKUP_NAME}.tar.gz" \
            "s3://${S3_BUCKET}/backups/${BACKUP_NAME}.tar.gz" \
            --region "$S3_REGION" \
            --storage-class STANDARD_IA

        log "S3 upload completed"
    else
        log_error "AWS CLI not found, skipping S3 upload"
    fi
else
    log "S3_BUCKET not set, skipping S3 upload"
fi

# ============================================================================
# 7. 오래된 백업 삭제 (로컬)
# ============================================================================
log "Cleaning up backups older than ${RETENTION_DAYS} days..."
find "$BACKUP_DIR" -name "zeptodb-backup-*.tar.gz" \
    -type f -mtime +${RETENTION_DAYS} -delete

# ============================================================================
# 8. S3 오래된 백업 삭제 (옵션)
# ============================================================================
if [ -n "$S3_BUCKET" ] && command -v aws &> /dev/null; then
    log "Cleaning up S3 backups older than ${RETENTION_DAYS} days..."

    CUTOFF_DATE=$(date -d "${RETENTION_DAYS} days ago" +%Y-%m-%d)

    aws s3 ls "s3://${S3_BUCKET}/backups/" | while read -r line; do
        BACKUP_DATE=$(echo "$line" | awk '{print $1}')
        BACKUP_FILE=$(echo "$line" | awk '{print $4}')

        if [[ "$BACKUP_DATE" < "$CUTOFF_DATE" ]]; then
            log "Deleting old S3 backup: $BACKUP_FILE"
            aws s3 rm "s3://${S3_BUCKET}/backups/${BACKUP_FILE}"
        fi
    done
fi

# ============================================================================
# 완료
# ============================================================================
log "Backup completed successfully: ${BACKUP_NAME}.tar.gz (${COMPRESSED_SIZE})"
exit 0
