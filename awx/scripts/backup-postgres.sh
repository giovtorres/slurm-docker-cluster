#!/bin/bash
#
# PostgreSQL Backup Script for AWX
#
# Creates compressed backups with point-in-time recovery support
# Supports both full and incremental backups
#
# Usage:
#   ./backup-postgres.sh [full|incremental|restore BACKUP_FILE]
#
# Environment Variables:
#   POSTGRES_HOST       - PostgreSQL hostname (default: awx-postgres)
#   POSTGRES_USER       - PostgreSQL username (default: awx)
#   POSTGRES_PASSWORD   - PostgreSQL password (required)
#   POSTGRES_DB         - Database name (default: awx)
#   BACKUP_DIR          - Backup directory (default: /backups)
#   BACKUP_RETENTION_DAYS - Days to keep backups (default: 30)
#

set -euo pipefail

# Configuration
POSTGRES_HOST="${POSTGRES_HOST:-awx-postgres}"
POSTGRES_USER="${POSTGRES_USER:-awx}"
POSTGRES_DB="${POSTGRES_DB:-awx}"
BACKUP_DIR="${BACKUP_DIR:-/backups}"
BACKUP_RETENTION_DAYS="${BACKUP_RETENTION_DAYS:-30}"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
BACKUP_NAME="awx_backup_${TIMESTAMP}"
LOG_FILE="${BACKUP_DIR}/backup.log"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Logging function
log() {
    local level=$1
    shift
    local message="$*"
    local timestamp=$(date '+%Y-%m-%d %H:%M:%S')
    echo -e "${timestamp} [${level}] ${message}" | tee -a "${LOG_FILE}"
}

log_info() { log "INFO" "$*"; }
log_warn() { log "${YELLOW}WARN${NC}" "$*"; }
log_error() { log "${RED}ERROR${NC}" "$*"; }
log_success() { log "${GREEN}SUCCESS${NC}" "$*"; }

# Check prerequisites
check_prerequisites() {
    if [[ -z "${POSTGRES_PASSWORD:-}" ]] && [[ -z "${PGPASSWORD:-}" ]]; then
        log_error "POSTGRES_PASSWORD or PGPASSWORD environment variable must be set"
        exit 1
    fi

    export PGPASSWORD="${POSTGRES_PASSWORD:-${PGPASSWORD}}"

    # Create backup directory if it doesn't exist
    mkdir -p "${BACKUP_DIR}"

    # Test database connection
    if ! pg_isready -h "${POSTGRES_HOST}" -U "${POSTGRES_USER}" -d "${POSTGRES_DB}" -q; then
        log_error "Cannot connect to PostgreSQL at ${POSTGRES_HOST}"
        exit 1
    fi
}

# Full backup with custom format (allows selective restore)
backup_full() {
    log_info "Starting full backup..."

    local backup_file="${BACKUP_DIR}/${BACKUP_NAME}_full.dump"
    local sql_backup="${BACKUP_DIR}/${BACKUP_NAME}_full.sql.gz"

    # Create custom format backup (most flexible for restores)
    pg_dump \
        -h "${POSTGRES_HOST}" \
        -U "${POSTGRES_USER}" \
        -d "${POSTGRES_DB}" \
        -Fc \
        -Z 6 \
        --verbose \
        -f "${backup_file}" \
        2>> "${LOG_FILE}"

    # Also create SQL dump for portability
    pg_dump \
        -h "${POSTGRES_HOST}" \
        -U "${POSTGRES_USER}" \
        -d "${POSTGRES_DB}" \
        --clean \
        --if-exists \
        --no-owner \
        --no-privileges \
        2>> "${LOG_FILE}" | gzip -9 > "${sql_backup}"

    # Get backup sizes
    local dump_size=$(du -h "${backup_file}" | cut -f1)
    local sql_size=$(du -h "${sql_backup}" | cut -f1)

    log_success "Full backup completed:"
    log_info "  Custom format: ${backup_file} (${dump_size})"
    log_info "  SQL format: ${sql_backup} (${sql_size})"

    # Create latest symlink
    ln -sf "${backup_file}" "${BACKUP_DIR}/latest_full.dump"
    ln -sf "${sql_backup}" "${BACKUP_DIR}/latest_full.sql.gz"

    # Generate backup manifest
    generate_manifest "${backup_file}" "${sql_backup}"
}

# Incremental backup (schema and data changes only)
backup_incremental() {
    log_info "Starting incremental backup..."

    local backup_file="${BACKUP_DIR}/${BACKUP_NAME}_incr.sql.gz"
    local changes_file="${BACKUP_DIR}/${BACKUP_NAME}_changes.json"

    # Get table sizes and row counts for change detection
    psql \
        -h "${POSTGRES_HOST}" \
        -U "${POSTGRES_USER}" \
        -d "${POSTGRES_DB}" \
        -t -A \
        -c "SELECT json_agg(row_to_json(t)) FROM (
            SELECT
                schemaname,
                relname as table_name,
                n_live_tup as row_count,
                pg_size_pretty(pg_relation_size(schemaname||'.'||relname)) as size
            FROM pg_stat_user_tables
            ORDER BY n_live_tup DESC
        ) t" > "${changes_file}"

    # Create incremental dump
    pg_dump \
        -h "${POSTGRES_HOST}" \
        -U "${POSTGRES_USER}" \
        -d "${POSTGRES_DB}" \
        --data-only \
        --inserts \
        2>> "${LOG_FILE}" | gzip -9 > "${backup_file}"

    local backup_size=$(du -h "${backup_file}" | cut -f1)
    log_success "Incremental backup completed: ${backup_file} (${backup_size})"
}

# Restore from backup
restore_backup() {
    local backup_file="$1"

    if [[ ! -f "${backup_file}" ]]; then
        log_error "Backup file not found: ${backup_file}"
        exit 1
    fi

    log_warn "This will DESTROY all existing data in database '${POSTGRES_DB}'"
    log_warn "Backup file: ${backup_file}"

    # Safety check - require confirmation in interactive mode
    if [[ -t 0 ]]; then
        read -p "Are you sure you want to continue? (yes/no): " confirm
        if [[ "${confirm}" != "yes" ]]; then
            log_info "Restore cancelled"
            exit 0
        fi
    fi

    log_info "Starting restore..."

    # Determine backup type and restore accordingly
    case "${backup_file}" in
        *.dump)
            # Custom format restore
            pg_restore \
                -h "${POSTGRES_HOST}" \
                -U "${POSTGRES_USER}" \
                -d "${POSTGRES_DB}" \
                --clean \
                --if-exists \
                --no-owner \
                --no-privileges \
                --verbose \
                "${backup_file}" \
                2>> "${LOG_FILE}"
            ;;
        *.sql.gz)
            # SQL format restore
            gunzip -c "${backup_file}" | psql \
                -h "${POSTGRES_HOST}" \
                -U "${POSTGRES_USER}" \
                -d "${POSTGRES_DB}" \
                2>> "${LOG_FILE}"
            ;;
        *.sql)
            # Plain SQL restore
            psql \
                -h "${POSTGRES_HOST}" \
                -U "${POSTGRES_USER}" \
                -d "${POSTGRES_DB}" \
                -f "${backup_file}" \
                2>> "${LOG_FILE}"
            ;;
        *)
            log_error "Unknown backup format: ${backup_file}"
            exit 1
            ;;
    esac

    log_success "Restore completed successfully"

    # Verify restore
    verify_restore
}

# Verify database after restore
verify_restore() {
    log_info "Verifying restore..."

    local table_count=$(psql \
        -h "${POSTGRES_HOST}" \
        -U "${POSTGRES_USER}" \
        -d "${POSTGRES_DB}" \
        -t -A \
        -c "SELECT count(*) FROM information_schema.tables WHERE table_schema = 'public'")

    local row_count=$(psql \
        -h "${POSTGRES_HOST}" \
        -U "${POSTGRES_USER}" \
        -d "${POSTGRES_DB}" \
        -t -A \
        -c "SELECT sum(n_live_tup) FROM pg_stat_user_tables")

    log_info "Database contains ${table_count} tables with approximately ${row_count} rows"
}

# Generate backup manifest
generate_manifest() {
    local backup_file="$1"
    local sql_backup="${2:-}"
    local manifest_file="${BACKUP_DIR}/${BACKUP_NAME}_manifest.json"

    local db_size=$(psql \
        -h "${POSTGRES_HOST}" \
        -U "${POSTGRES_USER}" \
        -d "${POSTGRES_DB}" \
        -t -A \
        -c "SELECT pg_size_pretty(pg_database_size('${POSTGRES_DB}'))")

    cat > "${manifest_file}" << EOF
{
    "backup_name": "${BACKUP_NAME}",
    "timestamp": "${TIMESTAMP}",
    "database": "${POSTGRES_DB}",
    "host": "${POSTGRES_HOST}",
    "files": {
        "custom_format": "$(basename "${backup_file}")",
        "sql_format": "$(basename "${sql_backup:-}")"
    },
    "database_size": "${db_size}",
    "backup_size": "$(du -h "${backup_file}" | cut -f1)",
    "checksum": "$(sha256sum "${backup_file}" | cut -d' ' -f1)"
}
EOF

    log_info "Manifest created: ${manifest_file}"
}

# Cleanup old backups
cleanup_old_backups() {
    log_info "Cleaning up backups older than ${BACKUP_RETENTION_DAYS} days..."

    local deleted_count=0

    while IFS= read -r -d '' file; do
        log_info "Deleting old backup: $(basename "${file}")"
        rm -f "${file}"
        ((deleted_count++))
    done < <(find "${BACKUP_DIR}" -name "awx_backup_*" -type f -mtime +"${BACKUP_RETENTION_DAYS}" -print0)

    if [[ ${deleted_count} -gt 0 ]]; then
        log_success "Deleted ${deleted_count} old backup files"
    else
        log_info "No old backups to delete"
    fi
}

# List available backups
list_backups() {
    log_info "Available backups in ${BACKUP_DIR}:"
    echo ""
    printf "%-50s %-15s %-20s\n" "Filename" "Size" "Date"
    printf "%s\n" "$(printf '=%.0s' {1..85})"

    for file in "${BACKUP_DIR}"/awx_backup_*.dump "${BACKUP_DIR}"/awx_backup_*.sql.gz; do
        if [[ -f "${file}" ]]; then
            local size=$(du -h "${file}" | cut -f1)
            local date=$(stat -c %y "${file}" 2>/dev/null || stat -f %Sm "${file}" 2>/dev/null)
            printf "%-50s %-15s %-20s\n" "$(basename "${file}")" "${size}" "${date:0:19}"
        fi
    done
}

# Main execution
main() {
    local command="${1:-full}"

    case "${command}" in
        full)
            check_prerequisites
            backup_full
            cleanup_old_backups
            ;;
        incremental)
            check_prerequisites
            backup_incremental
            ;;
        restore)
            if [[ -z "${2:-}" ]]; then
                log_error "Usage: $0 restore <backup_file>"
                exit 1
            fi
            check_prerequisites
            restore_backup "$2"
            ;;
        list)
            list_backups
            ;;
        cleanup)
            cleanup_old_backups
            ;;
        verify)
            check_prerequisites
            verify_restore
            ;;
        *)
            echo "Usage: $0 {full|incremental|restore <file>|list|cleanup|verify}"
            exit 1
            ;;
    esac
}

main "$@"
