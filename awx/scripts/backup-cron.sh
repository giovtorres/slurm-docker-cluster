#!/bin/bash
#
# AWX PostgreSQL Backup Cron Script
# Runs as part of the awx-backup container
#
# Schedule: Daily at 2:00 AM (configured in docker-compose)
# Retention: Configurable via BACKUP_RETENTION_DAYS env var
#

set -euo pipefail

BACKUP_DIR="/backups"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
LOG_FILE="${BACKUP_DIR}/cron_backup.log"

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" >> "${LOG_FILE}"
}

log "Starting scheduled backup..."

# Wait for PostgreSQL to be ready
until pg_isready -h "${POSTGRES_HOST}" -U "${POSTGRES_USER}" -d "${POSTGRES_DB}" -q; do
    log "Waiting for PostgreSQL..."
    sleep 5
done

# Create backup directory structure
mkdir -p "${BACKUP_DIR}/daily"
mkdir -p "${BACKUP_DIR}/weekly"
mkdir -p "${BACKUP_DIR}/monthly"

# Determine backup type based on day
DAY_OF_WEEK=$(date +%u)  # 1=Monday, 7=Sunday
DAY_OF_MONTH=$(date +%d)

BACKUP_FILE="${BACKUP_DIR}/daily/awx_${TIMESTAMP}.dump"

# Create custom format backup
pg_dump \
    -h "${POSTGRES_HOST}" \
    -U "${POSTGRES_USER}" \
    -d "${POSTGRES_DB}" \
    -Fc \
    -Z 6 \
    -f "${BACKUP_FILE}" \
    2>> "${LOG_FILE}"

if [[ $? -eq 0 ]]; then
    BACKUP_SIZE=$(du -h "${BACKUP_FILE}" | cut -f1)
    log "Daily backup completed: ${BACKUP_FILE} (${BACKUP_SIZE})"

    # Weekly backup on Sunday
    if [[ "${DAY_OF_WEEK}" == "7" ]]; then
        cp "${BACKUP_FILE}" "${BACKUP_DIR}/weekly/awx_weekly_${TIMESTAMP}.dump"
        log "Weekly backup created"
    fi

    # Monthly backup on the 1st
    if [[ "${DAY_OF_MONTH}" == "01" ]]; then
        cp "${BACKUP_FILE}" "${BACKUP_DIR}/monthly/awx_monthly_${TIMESTAMP}.dump"
        log "Monthly backup created"
    fi

    # Cleanup old backups
    # Keep daily for 7 days
    find "${BACKUP_DIR}/daily" -name "awx_*.dump" -mtime +7 -delete 2>/dev/null || true

    # Keep weekly for 4 weeks
    find "${BACKUP_DIR}/weekly" -name "awx_weekly_*.dump" -mtime +28 -delete 2>/dev/null || true

    # Keep monthly for retention period
    find "${BACKUP_DIR}/monthly" -name "awx_monthly_*.dump" -mtime +"${BACKUP_RETENTION_DAYS}" -delete 2>/dev/null || true

    log "Backup cleanup completed"
else
    log "ERROR: Backup failed!"
    exit 1
fi

log "Scheduled backup finished"
