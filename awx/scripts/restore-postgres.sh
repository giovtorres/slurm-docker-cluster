#!/bin/bash
#
# PostgreSQL Restore Script for AWX
#
# Restores AWX database from backup with safety checks
#
# Usage:
#   ./restore-postgres.sh <backup_file>
#   ./restore-postgres.sh --list
#   ./restore-postgres.sh --latest
#
# Examples:
#   ./restore-postgres.sh /backups/daily/awx_20240115_020000.dump
#   ./restore-postgres.sh --latest
#

set -euo pipefail

# Configuration
POSTGRES_HOST="${POSTGRES_HOST:-awx-postgres}"
POSTGRES_USER="${POSTGRES_USER:-awx}"
POSTGRES_DB="${POSTGRES_DB:-awx}"
BACKUP_DIR="${BACKUP_DIR:-/backups}"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

print_header() {
    echo -e "${CYAN}"
    echo "========================================"
    echo "  AWX PostgreSQL Restore Tool"
    echo "========================================"
    echo -e "${NC}"
}

log_info() { echo -e "${CYAN}[INFO]${NC} $*"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }
log_error() { echo -e "${RED}[ERROR]${NC} $*"; }
log_success() { echo -e "${GREEN}[SUCCESS]${NC} $*"; }

list_backups() {
    echo -e "\n${CYAN}Available Backups:${NC}\n"

    echo -e "${YELLOW}Daily Backups:${NC}"
    if [[ -d "${BACKUP_DIR}/daily" ]]; then
        ls -lh "${BACKUP_DIR}/daily"/*.dump 2>/dev/null | tail -10 || echo "  No daily backups found"
    fi

    echo -e "\n${YELLOW}Weekly Backups:${NC}"
    if [[ -d "${BACKUP_DIR}/weekly" ]]; then
        ls -lh "${BACKUP_DIR}/weekly"/*.dump 2>/dev/null || echo "  No weekly backups found"
    fi

    echo -e "\n${YELLOW}Monthly Backups:${NC}"
    if [[ -d "${BACKUP_DIR}/monthly" ]]; then
        ls -lh "${BACKUP_DIR}/monthly"/*.dump 2>/dev/null || echo "  No monthly backups found"
    fi
}

get_latest_backup() {
    # Find the most recent backup file
    local latest=""

    # Check daily first
    if [[ -d "${BACKUP_DIR}/daily" ]]; then
        latest=$(ls -t "${BACKUP_DIR}/daily"/*.dump 2>/dev/null | head -1)
    fi

    # Fall back to root backups
    if [[ -z "${latest}" ]] && [[ -f "${BACKUP_DIR}/latest_full.dump" ]]; then
        latest="${BACKUP_DIR}/latest_full.dump"
    fi

    if [[ -z "${latest}" ]]; then
        log_error "No backup files found"
        exit 1
    fi

    echo "${latest}"
}

pre_restore_checks() {
    local backup_file="$1"

    # Check if backup file exists
    if [[ ! -f "${backup_file}" ]]; then
        log_error "Backup file not found: ${backup_file}"
        exit 1
    fi

    # Check PostgreSQL connection
    if [[ -z "${PGPASSWORD:-}" ]] && [[ -z "${POSTGRES_PASSWORD:-}" ]]; then
        log_error "POSTGRES_PASSWORD or PGPASSWORD must be set"
        exit 1
    fi
    export PGPASSWORD="${POSTGRES_PASSWORD:-${PGPASSWORD}}"

    if ! pg_isready -h "${POSTGRES_HOST}" -U "${POSTGRES_USER}" -d "${POSTGRES_DB}" -q; then
        log_error "Cannot connect to PostgreSQL at ${POSTGRES_HOST}"
        exit 1
    fi

    # Show backup info
    local backup_size=$(du -h "${backup_file}" | cut -f1)
    local backup_date=$(stat -c %y "${backup_file}" 2>/dev/null || stat -f %Sm "${backup_file}" 2>/dev/null)

    echo ""
    log_info "Backup file: ${backup_file}"
    log_info "Backup size: ${backup_size}"
    log_info "Backup date: ${backup_date}"
    echo ""
}

stop_awx_services() {
    log_info "Stopping AWX services to prevent data corruption..."

    # This would typically stop the AWX web and task containers
    # In Docker Compose environment, this is done externally
    log_warn "Please ensure AWX web and task containers are stopped!"
    log_warn "Run: docker compose stop awx-web awx-task"
    echo ""

    read -p "Have you stopped the AWX services? (yes/no): " confirm
    if [[ "${confirm}" != "yes" ]]; then
        log_error "Restore cancelled - please stop AWX services first"
        exit 1
    fi
}

create_pre_restore_backup() {
    log_info "Creating pre-restore backup..."

    local pre_restore_file="${BACKUP_DIR}/pre_restore_$(date +%Y%m%d_%H%M%S).dump"

    pg_dump \
        -h "${POSTGRES_HOST}" \
        -U "${POSTGRES_USER}" \
        -d "${POSTGRES_DB}" \
        -Fc \
        -Z 6 \
        -f "${pre_restore_file}"

    log_success "Pre-restore backup created: ${pre_restore_file}"
}

perform_restore() {
    local backup_file="$1"

    log_info "Starting database restore..."

    # Drop and recreate connections
    psql -h "${POSTGRES_HOST}" -U "${POSTGRES_USER}" -d postgres -c "
        SELECT pg_terminate_backend(pid)
        FROM pg_stat_activity
        WHERE datname = '${POSTGRES_DB}' AND pid <> pg_backend_pid();
    " 2>/dev/null || true

    # Restore the backup
    pg_restore \
        -h "${POSTGRES_HOST}" \
        -U "${POSTGRES_USER}" \
        -d "${POSTGRES_DB}" \
        --clean \
        --if-exists \
        --no-owner \
        --no-privileges \
        --verbose \
        "${backup_file}" 2>&1 | while read line; do
            echo "  ${line}"
        done

    log_success "Database restore completed"
}

verify_restore() {
    log_info "Verifying restored database..."

    local table_count=$(psql \
        -h "${POSTGRES_HOST}" \
        -U "${POSTGRES_USER}" \
        -d "${POSTGRES_DB}" \
        -t -A \
        -c "SELECT count(*) FROM information_schema.tables WHERE table_schema = 'public'")

    local user_count=$(psql \
        -h "${POSTGRES_HOST}" \
        -U "${POSTGRES_USER}" \
        -d "${POSTGRES_DB}" \
        -t -A \
        -c "SELECT count(*) FROM auth_user" 2>/dev/null || echo "0")

    echo ""
    log_info "Verification results:"
    log_info "  Tables: ${table_count}"
    log_info "  Users: ${user_count}"
    echo ""

    if [[ "${table_count}" -gt 0 ]]; then
        log_success "Database verification passed"
    else
        log_error "Database appears empty - restore may have failed"
        exit 1
    fi
}

post_restore_instructions() {
    echo ""
    echo -e "${CYAN}========================================"
    echo "  Post-Restore Instructions"
    echo -e "========================================${NC}"
    echo ""
    echo "1. Start AWX services:"
    echo "   docker compose start awx-web awx-task"
    echo ""
    echo "2. Wait for services to be healthy:"
    echo "   docker compose ps"
    echo ""
    echo "3. Verify AWX is accessible:"
    echo "   curl -k https://localhost/api/v2/ping/"
    echo ""
    echo "4. Check AWX logs for any issues:"
    echo "   docker compose logs -f awx-web awx-task"
    echo ""
}

main() {
    print_header

    case "${1:-}" in
        --list|-l)
            list_backups
            exit 0
            ;;
        --latest)
            local backup_file=$(get_latest_backup)
            log_info "Using latest backup: ${backup_file}"
            ;;
        --help|-h)
            echo "Usage: $0 <backup_file> | --list | --latest | --help"
            echo ""
            echo "Options:"
            echo "  <backup_file>  Path to backup file to restore"
            echo "  --list, -l     List available backups"
            echo "  --latest       Restore from most recent backup"
            echo "  --help, -h     Show this help message"
            exit 0
            ;;
        "")
            log_error "No backup file specified"
            echo "Usage: $0 <backup_file> | --list | --latest"
            exit 1
            ;;
        *)
            local backup_file="$1"
            ;;
    esac

    # Run restore process
    pre_restore_checks "${backup_file}"

    echo -e "${RED}WARNING: This will OVERWRITE all data in the '${POSTGRES_DB}' database!${NC}"
    echo ""
    read -p "Are you sure you want to continue? (yes/no): " confirm

    if [[ "${confirm}" != "yes" ]]; then
        log_info "Restore cancelled"
        exit 0
    fi

    stop_awx_services
    create_pre_restore_backup
    perform_restore "${backup_file}"
    verify_restore
    post_restore_instructions

    log_success "Restore process completed successfully!"
}

main "$@"
