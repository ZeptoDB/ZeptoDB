#!/bin/bash
# ============================================================================
# ZeptoDB 프로덕션 서비스 설치 스크립트
# ============================================================================
# 용도: systemd 서비스, 사용자, 디렉토리, cron 설정
# 실행: sudo ./install_service.sh
# ============================================================================

set -euo pipefail

# 권한 체크
if [ "$EUID" -ne 0 ]; then
    echo "Error: This script must be run as root"
    exit 1
fi

# ============================================================================
# 설정
# ============================================================================
APEX_HOME="/opt/zeptodb"
DATA_DIR="/data/zeptodb"
LOG_DIR="/var/log/zeptodb"
BACKUP_DIR="/backup/zeptodb"

# ============================================================================
# 1. zeptodb 사용자 생성
# ============================================================================
echo "[1/8] Creating zeptodb user..."
if ! id -u zeptodb > /dev/null 2>&1; then
    useradd -r -u 1000 zeptodb
    echo "✓ User 'zeptodb' created"
else
    echo "✓ User 'zeptodb' already exists"
fi

# ============================================================================
# 2. 디렉토리 생성
# ============================================================================
echo "[2/8] Creating directories..."
mkdir -p "$APEX_HOME"
mkdir -p "$DATA_DIR"/{hdb,wal,tmp}
mkdir -p "$LOG_DIR"
mkdir -p "$BACKUP_DIR"

chown -R zeptodb:zeptodb "$APEX_HOME"
chown -R zeptodb:zeptodb "$DATA_DIR"
chown -R zeptodb:zeptodb "$LOG_DIR"
chown -R zeptodb:zeptodb "$BACKUP_DIR"

echo "✓ Directories created"

# ============================================================================
# 3. 바이너리 복사
# ============================================================================
echo "[3/8] Installing binaries..."
if [ -f "./build/zepto_server" ]; then
    mkdir -p "${APEX_HOME}/bin"
    cp ./build/zepto_server "${APEX_HOME}/bin/"
    chmod +x "${APEX_HOME}/bin/zepto_server"
    echo "✓ Binaries installed"
else
    echo "⚠ Warning: zepto_server binary not found in ./build/"
fi

# ============================================================================
# 4. 설정 파일 복사
# ============================================================================
echo "[4/8] Installing config..."
if [ -f "./config.yaml" ]; then
    cp ./config.yaml "${APEX_HOME}/config.yaml"
    chown zeptodb:zeptodb "${APEX_HOME}/config.yaml"
    echo "✓ Config installed"
else
    echo "⚠ Warning: config.yaml not found"
fi

# ============================================================================
# 5. 스크립트 복사
# ============================================================================
echo "[5/8] Installing scripts..."
mkdir -p "${APEX_HOME}/scripts"
cp scripts/*.sh "${APEX_HOME}/scripts/"
chmod +x "${APEX_HOME}/scripts/"*.sh
chown -R zeptodb:zeptodb "${APEX_HOME}/scripts"
echo "✓ Scripts installed"

# ============================================================================
# 6. systemd 서비스 설치
# ============================================================================
echo "[6/8] Installing systemd service..."
cp scripts/zeptodb.service /etc/systemd/system/zeptodb.service
systemctl daemon-reload
systemctl enable zeptodb.service
echo "✓ systemd service installed"

# ============================================================================
# 7. cron 작업 설치
# ============================================================================
echo "[7/8] Installing cron jobs..."

# EOD 프로세스 (평일 18:00)
(crontab -u zeptodb -l 2>/dev/null || true; echo "0 18 * * 1-5 ${APEX_HOME}/scripts/eod_process.sh") | crontab -u zeptodb -

# 백업 (매일 02:00)
(crontab -u zeptodb -l 2>/dev/null || true; echo "0 2 * * * ${APEX_HOME}/scripts/backup.sh") | crontab -u zeptodb -

echo "✓ Cron jobs installed"

# ============================================================================
# 8. 로그 로테이션 설정
# ============================================================================
echo "[8/8] Configuring log rotation..."
cat > /etc/logrotate.d/zeptodb <<EOF
${LOG_DIR}/*.log {
    daily
    rotate 30
    missingok
    notifempty
    compress
    delaycompress
    copytruncate
    create 0644 zeptodb apex
}
EOF
echo "✓ Log rotation configured"

# ============================================================================
# 완료
# ============================================================================
echo ""
echo "========================================="
echo "ZeptoDB Installation Complete!"
echo "========================================="
echo ""
echo "Next steps:"
echo "  1. Review config: ${APEX_HOME}/config.yaml"
echo "  2. Start service: sudo systemctl start zeptodb"
echo "  3. Check status:  sudo systemctl status zeptodb"
echo "  4. View logs:     sudo journalctl -u zeptodb -f"
echo ""
echo "Endpoints:"
echo "  HTTP API:  http://localhost:8123/"
echo "  Health:    http://localhost:8123/health"
echo "  Metrics:   http://localhost:8123/metrics"
echo ""
