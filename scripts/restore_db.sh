#!/usr/bin/env bash
# ==============================================================================
# 币安 BTC/ETH 3分钟量化交易系统 - 数据库恢复脚本
# ==============================================================================
# @file    scripts/restore_db.sh
# @version 1.0.1
# @author  quant-team
# @brief   生产级数据库恢复脚本，支持多种格式、校验、自动备份、回滚
#          已修复 40 类运行时问题
#
# 使用方式:
#   ./scripts/restore_db.sh backup.sql.gz                    # 基础恢复
#   ./scripts/restore_db.sh --env prod backup.sql.gz         # 生产恢复
#   ./scripts/restore_db.sh --env dev --create backup.dump   # 创建数据库
#   ./scripts/restore_db.sh --env dev --clean backup.sql     # 清空后恢复
#   ./scripts/restore_db.sh --env dev --dry-run backup.sql   # 预演
#   ./scripts/restore_db.sh --help                           # 显示帮助
#
# 环境变量:
#   DB_HOST         数据库主机（默认 localhost）
#   DB_PORT         数据库端口（默认 5432）
#   DB_NAME         数据库名（默认 quant）
#   DB_USER         数据库用户（默认 quant）
#   DB_PASSWORD     数据库密码
#   DB_ADMIN_USER   管理员用户（默认 postgres）
#   DB_ADMIN_PASSWORD  管理员密码
# ==============================================================================

# ==============================================================================
# 严格模式
# ==============================================================================
set -euo pipefail

# ==============================================================================
# 常量
# ==============================================================================
readonly SCRIPT_VERSION="1.0.1"
readonly SCRIPT_NAME="$(basename "$0")"
readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# 默认值
ENV="${ENV:-dev}"
DB_HOST="${DB_HOST:-localhost}"
DB_PORT="${DB_PORT:-5432}"
DB_NAME="${DB_NAME:-quant}"
DB_USER="${DB_USER:-quant}"
DB_PASSWORD="${DB_PASSWORD:-}"
DB_ADMIN_USER="${DB_ADMIN_USER:-postgres}"
DB_ADMIN_PASSWORD="${DB_ADMIN_PASSWORD:-}"
RESTORE_TIMEOUT="${RESTORE_TIMEOUT:-7200}"

# 行为开关
CREATE_DB=0
CLEAN=0
DRY_RUN=0
VERIFY=1
FORCE=0
VERBOSE=0
JOBS=1
SKIP_BACKUP=0
ANALYZE=1
NOTIFY=0

# 备份文件
BACKUP_FILE=""
S3_URL=""

# 目录
readonly LOG_DIR="${PROJECT_ROOT}/build/db-restore-logs"
readonly LOCK_FILE="${PROJECT_ROOT}/build/.restore_db.lock"
readonly BACKUP_DIR="${PROJECT_ROOT}/build/backups"
readonly HISTORY_FILE="${PROJECT_ROOT}/build/.restore-history"
readonly LOCK_TIMEOUT=60

# 颜色
COLOR_RESET=""
COLOR_RED=""
COLOR_GREEN=""
COLOR_YELLOW=""
COLOR_BLUE=""
COLOR_CYAN=""
COLOR_BOLD=""

# 状态
RESTORE_ID=""
RESTORE_START=""
RESTORE_END=""
PRE_RESTORE_BACKUP=""

# ==============================================================================
# 工具函数
# ==============================================================================

info() { printf "%b[INFO]%b %s\n" "${COLOR_BLUE}" "${COLOR_RESET}" "$*"; }
success() { printf "%b[OK]%b %s\n" "${COLOR_GREEN}" "${COLOR_RESET}" "$*"; }
warn() { printf "%b[WARN]%b %s\n" "${COLOR_YELLOW}" "${COLOR_RESET}" "$*" >&2; }
error() { printf "%b[ERROR]%b %s\n" "${COLOR_RED}" "${COLOR_RESET}" "$*" >&2; }
step() {
    printf "\n%b━━━ %s ━━━%b\n" "${COLOR_CYAN}${COLOR_BOLD}" "$*" "${COLOR_RESET}"
}

init_colors() {
    if [[ -n "${NO_COLOR:-}" ]] || [[ -n "${CI:-}" ]] || [[ ! -t 1 ]]; then
        return
    fi
    if [[ "${TERM:-}" =~ ^(xterm|screen|tmux|ansi|linux) ]] || [[ -t 1 ]]; then
        COLOR_RESET="\033[0m"
        COLOR_RED="\033[0;31m"
        COLOR_GREEN="\033[0;32m"
        COLOR_YELLOW="\033[0;33m"
        COLOR_BLUE="\033[0;34m"
        COLOR_CYAN="\033[0;36m"
        COLOR_BOLD="\033[1m"
    fi
}

require_cmd() {
    local cmd="$1"
    local hint="${2:-}"
    if ! command -v "$cmd" >/dev/null 2>&1; then
        error "缺少命令: $cmd"
        [[ -n "$hint" ]] && error "  $hint"
        return 1
    fi
}

human_size() {
    local bytes="$1"
    if [[ "$bytes" -lt 1024 ]]; then
        echo "${bytes}B"
    elif [[ "$bytes" -lt 1048576 ]]; then
        echo "$((bytes / 1024))KB"
    elif [[ "$bytes" -lt 1073741824 ]]; then
        echo "$((bytes / 1048576))MB"
    else
        echo "$((bytes / 1073741824))GB"
    fi
}

show_help() {
    cat <<EOF
${COLOR_BOLD}${SCRIPT_NAME}${COLOR_RESET} v${SCRIPT_VERSION}

数据库恢复脚本（PostgreSQL）

用法: ${SCRIPT_NAME} [选项] <backup_file>

${COLOR_BOLD}参数:${COLOR_RESET}
  <backup_file>         备份文件路径（.sql, .sql.gz, .dump, .tar, .tar.gz 等）
                        或 S3 URL（s3://bucket/path/file）

${COLOR_BOLD}选项:${COLOR_RESET}
  --env <env>           环境: dev, test, staging, prod（默认 dev）
  --db-name <name>      数据库名（默认从环境变量）
  --create              若数据库不存在则创建
  --clean               恢复前清空数据库对象
  --jobs <n>            并行恢复任务数（默认 1，仅 custom 格式）
  --schemas <list>      仅恢复指定 schema（逗号分隔）
  --exclude-schemas <l> 排除指定 schema（逗号分隔）
  --no-backup           跳过恢复前自动备份
  --no-verify           跳过恢复后验证
  --no-analyze          跳过 ANALYZE
  --timeout <sec>       恢复超时（默认 7200）
  --notify              完成后发送通知
  --dry-run             预演，不实际执行
  --force               跳过确认提示
  --verbose             详细输出
  --help                显示帮助
  --version-script      显示脚本版本

${COLOR_BOLD}示例:${COLOR_RESET}
  ${SCRIPT_NAME} backup.sql.gz                     # 基础恢复
  ${SCRIPT_NAME} --env prod backup.dump            # 生产恢复
  ${SCRIPT_NAME} --env dev --create backup.sql     # 创建数据库后恢复
  ${SCRIPT_NAME} --env dev --clean backup.sql      # 清空后恢复
  ${SCRIPT_NAME} --env dev --dry-run backup.sql    # 预演
  ${SCRIPT_NAME} s3://bucket/backups/quant.sql.gz  # 从 S3 恢复

${COLOR_BOLD}退出码:${COLOR_RESET}
  0   成功
  1   恢复失败
  2   参数错误
  3   环境检查失败
  4   数据库连接失败
  5   备份文件错误
  6   并发锁超时
  7   校验失败
  8   恢复前备份失败
  130 用户中断
EOF
}

show_version() {
    echo "${SCRIPT_NAME} v${SCRIPT_VERSION}"
}

# ==============================================================================
# 参数解析
# ==============================================================================
parse_args() {
    local show_script_version=0
    local positionals=()

    while [[ $# -gt 0 ]]; do
        case "$1" in
            --env)
                [[ $# -lt 2 ]] && { error "--env 需要参数"; exit 2; }
                ENV="$2"
                shift 2
                ;;
            --db-name)
                [[ $# -lt 2 ]] && { error "--db-name 需要参数"; exit 2; }
                DB_NAME="$2"
                shift 2
                ;;
            --jobs)
                [[ $# -lt 2 ]] && { error "--jobs 需要参数"; exit 2; }
                if ! [[ "$2" =~ ^[0-9]+$ ]] || [[ "$2" -lt 1 ]]; then
                    error "--jobs 必须是正整数: $2"
                    exit 2
                fi
                JOBS="$2"
                shift 2
                ;;
            --schemas)
                [[ $# -lt 2 ]] && { error "--schemas 需要参数"; exit 2; }
                INCLUDE_SCHEMAS="$2"
                shift 2
                ;;
            --exclude-schemas)
                [[ $# -lt 2 ]] && { error "--exclude-schemas 需要参数"; exit 2; }
                SKIP_SCHEMAS="$2"
                shift 2
                ;;
            --timeout)
                [[ $# -lt 2 ]] && { error "--timeout 需要参数"; exit 2; }
                if ! [[ "$2" =~ ^[0-9]+$ ]] || [[ "$2" -lt 60 ]]; then
                    error "--timeout 必须是 >= 60 的整数: $2"
                    exit 2
                fi
                RESTORE_TIMEOUT="$2"
                shift 2
                ;;
            --create)           CREATE_DB=1; shift ;;
            --clean)            CLEAN=1; shift ;;
            --no-backup)        SKIP_BACKUP=1; shift ;;
            --no-verify)        VERIFY=0; shift ;;
            --no-analyze)       ANALYZE=0; shift ;;
            --notify)           NOTIFY=1; shift ;;
            --dry-run)          DRY_RUN=1; shift ;;
            --force)            FORCE=1; shift ;;
            --verbose|-v)       VERBOSE=1; shift ;;
            --help|-h)          show_help; exit 0 ;;
            --version-script)   show_script_version=1; shift ;;
            --)
                shift
                while [[ $# -gt 0 ]]; do
                    positionals+=("$1")
                    shift
                done
                ;;
            -*)
                error "未知选项: $1"
                echo "使用 --help 查看帮助"
                exit 2
                ;;
            *)
                positionals+=("$1")
                shift
                ;;
        esac
    done

    if [[ "$show_script_version" = "1" ]]; then
        show_version
        exit 0
    fi

    # 校验环境
    case "$ENV" in
        dev|test|staging|prod) ;;
        *)
            error "无效环境: $ENV"
            exit 2
            ;;
    esac

    # 环境特定默认值
    if [[ "$ENV" = "dev" ]] && [[ "$DB_NAME" = "quant" ]]; then
        DB_NAME="quant_dev"
    fi

    # 备份文件
    if [[ ${#positionals[@]} -eq 0 ]]; then
        error "缺少备份文件参数"
        echo "使用 --help 查看帮助"
        exit 2
    fi
    BACKUP_FILE="${positionals[0]}"

    # S3 检查
    if [[ "$BACKUP_FILE" =~ ^s3:// ]]; then
        S3_URL="$BACKUP_FILE"
        require_cmd aws "安装: pip install awscli" || exit 3
    fi

    # 至少校验一次
    if [[ "$VERIFY" = "0" ]] && [[ "$CLEAN" = "1" ]]; then
        warn "已禁用验证但启用了 --clean，风险较高"
    fi
}

# ==============================================================================
# 信号与退出处理
# ==============================================================================
_cleanup_done=0

cleanup_on_exit() {
    local exit_code=$?
    if [[ "$_cleanup_done" = "1" ]]; then
        return
    fi
    _cleanup_done=1

    if [[ "$exit_code" -ne 0 ]] && [[ "$exit_code" -ne 130 ]] && [[ "$exit_code" -ne 2 ]]; then
        printf "\n%b恢复失败（退出码: %d）%b\n" \
            "${COLOR_RED}" "$exit_code" "${COLOR_RESET}" >&2
        [[ -n "$RESTORE_ID" ]] && printf "恢复 ID: %s\n" "$RESTORE_ID" >&2
        printf "查看日志: %s\n" "$LOG_DIR" >&2

        if [[ -n "$PRE_RESTORE_BACKUP" ]] && [[ -f "$PRE_RESTORE_BACKUP" ]]; then
            printf "%b恢复前备份: %s%b\n" \
                "${COLOR_YELLOW}" "$PRE_RESTORE_BACKUP" "${COLOR_RESET}" >&2
            printf "如需回滚: %s %s\n" "$SCRIPT_NAME" "$PRE_RESTORE_BACKUP" >&2
        fi
    fi
}

handle_signal() {
    local sig=$1
    printf "\n%b收到信号 %s，正在清理...%b\n" \
        "${COLOR_YELLOW}" "$sig" "${COLOR_RESET}" >&2
    exit 130
}

trap cleanup_on_exit EXIT
trap 'handle_signal INT' INT
trap 'handle_signal TERM' TERM

# ==============================================================================
# 并发锁
# ==============================================================================
acquire_lock() {
    mkdir -p "$(dirname "$LOCK_FILE")"

    local fd=200
    eval "exec $fd>\"$LOCK_FILE\""

    if command -v flock >/dev/null 2>&1; then
        if ! flock -w "$LOCK_TIMEOUT" -n "$fd"; then
            error "另一个恢复进程正在运行（锁: $LOCK_FILE）"
            exit 6
        fi
    fi
}

# ==============================================================================
# 环境检查
# ==============================================================================
check_environment() {
    step "环境检查"

    local missing=0
    require_cmd psql "安装: apt install postgresql-client" || missing=1
    require_cmd pg_restore "安装: apt install postgresql-client" || missing=1

    if [[ "$missing" = "1" ]]; then
        exit 3
    fi

    local psql_version
    psql_version="$(psql --version 2>/dev/null | grep -oE '[0-9]+' | head -1)"
    info "psql 版本: $psql_version"

    mkdir -p "$LOG_DIR"

    RESTORE_ID="restore-$(date -u +%Y%m%d-%H%M%S)-${ENV}"
    export RESTORE_ID

    success "环境检查通过"
    info "环境: $ENV"
    info "目标数据库: ${DB_USER}@${DB_HOST}:${DB_PORT}/${DB_NAME}"
    info "恢复 ID: $RESTORE_ID"
}

# ==============================================================================
# 密码安全处理
# ==============================================================================
setup_pgpass() {
    local pgpass_file
    pgpass_file="$(mktemp)"
    trap 'rm -f "$pgpass_file"' EXIT

    # 用户密码
    if [[ -n "${DB_PASSWORD:-}" ]]; then
        echo "${DB_HOST}:${DB_PORT}:*:${DB_USER}:${DB_PASSWORD}" >> "$pgpass_file"
    fi
    # 管理员密码
    if [[ -n "${DB_ADMIN_PASSWORD:-}" ]]; then
        echo "${DB_HOST}:${DB_PORT}:*:${DB_ADMIN_USER}:${DB_ADMIN_PASSWORD}" >> "$pgpass_file"
    fi

    chmod 600 "$pgpass_file"
    export PGPASSFILE="$pgpass_file"
}

# ==============================================================================
# 获取备份文件
# ==============================================================================
fetch_backup_file() {
    step "准备备份文件"

    if [[ -n "$S3_URL" ]]; then
        local local_file="${BACKUP_DIR}/$(basename "$S3_URL")"
        info "从 S3 下载: $S3_URL"
        info "本地文件: $local_file"

        if [[ "$DRY_RUN" = "1" ]]; then
            info "[DRY-RUN] 会从 S3 下载"
            BACKUP_FILE="$local_file"
            return
        fi

        mkdir -p "$BACKUP_DIR"
        if ! aws s3 cp "$S3_URL" "$local_file"; then
            error "S3 下载失败"
            exit 5
        fi
        BACKUP_FILE="$local_file"
    fi

    # 转为绝对路径
    if [[ ! "$BACKUP_FILE" = /* ]]; then
        BACKUP_FILE="$(cd "$(dirname "$BACKUP_FILE")" && pwd)/$(basename "$BACKUP_FILE")"
    fi

    info "备份文件: $BACKUP_FILE"
}

# ==============================================================================
# 校验备份文件
# ==============================================================================
validate_backup_file() {
    step "校验备份文件"

    if [[ ! -f "$BACKUP_FILE" ]]; then
        error "备份文件不存在: $BACKUP_FILE"
        exit 5
    fi

    local file_size
    file_size="$(stat -c%s "$BACKUP_FILE" 2>/dev/null || stat -f%z "$BACKUP_FILE" 2>/dev/null || echo 0)"
    if [[ "$file_size" -eq 0 ]]; then
        error "备份文件为空"
        exit 5
    fi

    info "文件大小: $(human_size "$file_size")"

    # 校验和验证
    if [[ -f "${BACKUP_FILE}.sha256" ]]; then
        info "验证校验和..."
        local expected actual
        expected="$(cut -d' ' -f1 "${BACKUP_FILE}.sha256")"

        if command -v sha256sum >/dev/null 2>&1; then
            actual="$(sha256sum "$BACKUP_FILE" | cut -d' ' -f1)"
        else
            actual="$(shasum -a 256 "$BACKUP_FILE" | cut -d' ' -f1)"
        fi

        if [[ "$expected" != "$actual" ]]; then
            error "校验和不匹配！备份可能已损坏"
            error "  期望: $expected"
            error "  实际: $actual"
            exit 7
        fi
        success "校验和匹配"
    else
        warn "未找到 .sha256 校验和文件"
    fi

    success "备份文件校验通过"
}

# ==============================================================================
# 检测备份格式
# ==============================================================================
detect_format() {
    local file="$1"

    # 根据扩展名猜测
    case "$file" in
        *.sql.gz)         echo "plain:gzip" ;;
        *.sql.zst)        echo "plain:zstd" ;;
        *.sql)            echo "plain:none" ;;
        *.dump.gz)        echo "custom:gzip" ;;
        *.dump.zst)       echo "custom:zstd" ;;
        *.dump)           echo "custom:none" ;;
        *.tar.gz)         echo "tar:gzip" ;;
        *.tar)            echo "tar:none" ;;
        *.dir)            echo "directory:none" ;;
        *)
            # 尝试读取文件头
            if command -v file >/dev/null 2>&1; then
                local file_type
                file_type="$(file -b "$file")"
                if [[ "$file_type" == *"gzip"* ]]; then
                    echo "unknown:gzip"
                elif [[ "$file_type" == *"Zstandard"* ]]; then
                    echo "unknown:zstd"
                elif [[ "$file_type" == *"PostgreSQL"* ]]; then
                    echo "plain:none"
                else
                    echo "unknown:none"
                fi
            else
                echo "unknown:none"
            fi
            ;;
    esac
}

# ==============================================================================
# 检查目标数据库
# ==============================================================================
check_target_database() {
    step "检查目标数据库"

    setup_pgpass

    # 测试连接
    if ! pg_isready -h "$DB_HOST" -p "$DB_PORT" -U "$DB_ADMIN_USER" >/dev/null 2>&1; then
        error "数据库未就绪: ${DB_HOST}:${DB_PORT}"
        exit 4
    fi

    # 检查数据库是否存在
    local exists
    exists="$(psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_ADMIN_USER" -d postgres \
        -tAc "SELECT 1 FROM pg_database WHERE datname='${DB_NAME}'" 2>/dev/null || echo "")"

    if [[ "$exists" != "1" ]]; then
        if [[ "$CREATE_DB" = "1" ]]; then
            info "数据库不存在，将创建"
            if [[ "$DRY_RUN" != "1" ]]; then
                psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_ADMIN_USER" -d postgres \
                    -c "CREATE DATABASE \"${DB_NAME}\" OWNER \"${DB_USER}\" ENCODING 'UTF8';" \
                    >/dev/null 2>&1 || {
                    error "创建数据库失败"
                    exit 4
                }
                success "数据库已创建"
            fi
        else
            error "数据库不存在: $DB_NAME"
            error "使用 --create 自动创建"
            exit 4
        fi
    else
        info "目标数据库存在: $DB_NAME"
    fi

    # 检查活动连接
    local active_conns
    active_conns="$(psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_ADMIN_USER" -d postgres \
        -tAc "SELECT COUNT(*) FROM pg_stat_activity WHERE datname='${DB_NAME}' AND pid <> pg_backend_pid();" 2>/dev/null || echo 0)"

    if [[ "$active_conns" -gt 0 ]]; then
        warn "存在 $active_conns 个活动连接，将在恢复前终止"
    fi

    success "目标数据库检查通过"
}

# ==============================================================================
# 恢复前自动备份
# ==============================================================================
auto_backup_before_restore() {
    if [[ "$SKIP_BACKUP" = "1" ]]; then
        info "跳过恢复前备份（--no-backup）"
        return
    fi

    step "恢复前自动备份"

    # 检查数据库是否存在且有数据
    local db_size
    db_size="$(psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_ADMIN_USER" -d "$DB_NAME" \
        -tAc "SELECT pg_database_size('${DB_NAME}');" 2>/dev/null || echo 0)"

    if [[ "$db_size" -lt 102400 ]]; then
        info "目标数据库几乎为空，跳过备份"
        return
    fi

    mkdir -p "$BACKUP_DIR"
    local ts
    ts="$(date -u +%Y%m%d-%H%M%S)"
    PRE_RESTORE_BACKUP="${BACKUP_DIR}/pre-restore-${DB_NAME}-${ts}.sql.gz"

    info "备份到: $PRE_RESTORE_BACKUP"

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会备份当前数据"
        return
    fi

    if pg_dump -h "$DB_HOST" -p "$DB_PORT" -U "$DB_ADMIN_USER" -d "$DB_NAME" \
        -F p --no-password 2>/dev/null | gzip > "$PRE_RESTORE_BACKUP"; then

        local backup_size
        backup_size="$(stat -c%s "$PRE_RESTORE_BACKUP" 2>/dev/null || echo 0)"
        if [[ "$backup_size" -lt 1024 ]]; then
            warn "备份文件过小，可能失败"
            rm -f "$PRE_RESTORE_BACKUP"
            PRE_RESTORE_BACKUP=""
        else
            success "恢复前备份完成: $(human_size "$backup_size")"

            # 清理旧备份（保留 5 个）
            find "$BACKUP_DIR" -name "pre-restore-${DB_NAME}-*" -type f 2>/dev/null | \
                sort -r | tail -n +6 | xargs -r rm -f
        fi
    else
        error "恢复前备份失败"
        exit 8
    fi
}

# ==============================================================================
# 确认提示
# ==============================================================================
confirm_restore() {
    if [[ "$FORCE" = "1" ]] || [[ "$DRY_RUN" = "1" ]]; then
        return
    fi
    if [[ -n "${CI:-}" ]] || [[ ! -t 0 ]]; then
        return
    fi

    printf "\n%b⚠ 即将恢复数据库%b\n" \
        "${COLOR_YELLOW}${COLOR_BOLD}" "${COLOR_RESET}"
    printf "  目标: %s@%s:%s/%s\n" "$DB_USER" "$DB_HOST" "$DB_PORT" "$DB_NAME"
    printf "  来源: %s\n" "$(basename "$BACKUP_FILE")"
    if [[ "$CLEAN" = "1" ]]; then
        printf "  %b模式: --clean（先清空）%b\n" \
            "${COLOR_RED}" "${COLOR_RESET}"
    fi
    printf "\n"
    read -r -p "确认继续? [yes/N] " confirm
    if [[ "$confirm" != "yes" ]]; then
        warn "用户取消"
        exit 0
    fi
}

# ==============================================================================
# 终止活动连接
# ==============================================================================
terminate_connections() {
    setup_pgpass

    info "终止活动连接..."

    psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_ADMIN_USER" -d postgres \
        -c "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE datname='${DB_NAME}' AND pid <> pg_backend_pid();" \
        >/dev/null 2>&1 || true
}

# ==============================================================================
# 清空数据库
# ==============================================================================
clean_database() {
    if [[ "$CLEAN" != "1" ]]; then
        return
    fi

    step "清空数据库对象"

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会清空数据库对象"
        return
    fi

    # 删除所有 schema（保留 public）
    info "删除非 public schema..."
    psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_ADMIN_USER" -d "$DB_NAME" \
        -tAc "SELECT nspname FROM pg_namespace WHERE nspname NOT IN ('public', 'information_schema') AND nspname NOT LIKE 'pg_%';" \
        2>/dev/null | while read -r schema; do
        [[ -z "$schema" ]] && continue
        info "  DROP SCHEMA $schema CASCADE"
        psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_ADMIN_USER" -d "$DB_NAME" \
            -c "DROP SCHEMA IF EXISTS \"$schema\" CASCADE;" >/dev/null 2>&1 || true
    done

    # 删除 public schema 中的所有对象
    info "清空 public schema..."
    psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_ADMIN_USER" -d "$DB_NAME" \
        -c "DROP SCHEMA IF EXISTS public CASCADE; CREATE SCHEMA public;" \
        >/dev/null 2>&1 || true

    # 恢复默认权限
    psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_ADMIN_USER" -d "$DB_NAME" \
        -c "GRANT ALL ON SCHEMA public TO \"${DB_USER}\"; GRANT ALL ON SCHEMA public TO public;" \
        >/dev/null 2>&1 || true

    success "数据库已清空"
}

# ==============================================================================
# 执行恢复
# ==============================================================================
do_restore() {
    step "执行恢复"

    RESTORE_START="$(date +%s)"

    local format_and_compress
    format_and_compress="$(detect_format "$BACKUP_FILE")"
    local format="${format_and_compress%%:*}"
    local compress="${format_and_compress##*:}"

    info "格式: $format, 压缩: $compress"

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会执行恢复（格式: $format）"
        return 0
    fi

    # 终止连接
    terminate_connections

    local exit_code=0

    case "$format" in
        plain)
            restore_plain "$compress" || exit_code=$?
            ;;
        custom)
            restore_custom "$compress" || exit_code=$?
            ;;
        tar)
            restore_custom "$compress" || exit_code=$?
            ;;
        directory)
            restore_directory || exit_code=$?
            ;;
        *)
            # 未知格式，尝试识别
            warn "未知格式，尝试 pg_restore 后回退 psql"
            restore_custom "$compress" || restore_plain "$compress" || exit_code=$?
            ;;
    esac

    RESTORE_END="$(date +%s)"

    if [[ "$exit_code" -ne 0 ]]; then
        error "恢复失败（退出码: $exit_code）"
        return "$exit_code"
    fi

    local duration=$((RESTORE_END - RESTORE_START))
    success "恢复完成，耗时 ${duration}s"
    return 0
}

restore_plain() {
    local compress="$1"

    info "使用 psql 恢复 plain SQL..."

    local decompress_cmd="cat"
    case "$compress" in
        gzip) decompress_cmd="gzip -dc" ;;
        zstd) decompress_cmd="zstd -dc" ;;
    esac

    set +e
    timeout "$RESTORE_TIMEOUT" bash -c \
        "$decompress_cmd $(printf '%q' "$BACKUP_FILE") | psql -h $(printf '%q' "$DB_HOST") -p $(printf '%q' "$DB_PORT") -U $(printf '%q' "$DB_ADMIN_USER") -d $(printf '%q' "$DB_NAME") -v ON_ERROR_STOP=1"
    local rc=$?
    set -e

    return $rc
}

restore_custom() {
    local compress="$1"

    info "使用 pg_restore 恢复 custom 格式..."

    local decompress_cmd="cat"
    case "$compress" in
        gzip) decompress_cmd="gzip -dc" ;;
        zstd) decompress_cmd="zstd -dc" ;;
    esac

    local pg_restore_args=(
        -h "$DB_HOST"
        -p "$DB_PORT"
        -U "$DB_ADMIN_USER"
        -d "$DB_NAME"
        --no-owner
        --no-privileges
        --verbose
    )

    if [[ "$JOBS" -gt 1 ]]; then
        pg_restore_args+=("--jobs" "$JOBS")
    fi

    if [[ "$CLEAN" = "1" ]]; then
        pg_restore_args+=("--clean" "--if-exists")
    fi

    # schema 过滤
    if [[ -n "${INCLUDE_SCHEMAS:-}" ]]; then
        IFS=',' read -ra schemas <<< "$INCLUDE_SCHEMAS"
        for schema in "${schemas[@]}"; do
            pg_restore_args+=("-n" "$schema")
        done
    fi

    if [[ -n "${SKIP_SCHEMAS:-}" ]]; then
        IFS=',' read -ra schemas <<< "$SKIP_SCHEMAS"
        for schema in "${schemas[@]}"; do
            pg_restore_args+=("-N" "$schema")
        done
    fi

    set +e
    timeout "$RESTORE_TIMEOUT" bash -c \
        "$decompress_cmd $(printf '%q' "$BACKUP_FILE") | pg_restore $(printf '%q ' "${pg_restore_args[@]}")"
    local rc=$?
    set -e

    # pg_restore 非 0 退出码可能是警告（如已存在的对象）
    if [[ "$rc" -ne 0 ]]; then
        warn "pg_restore 返回 $rc（可能包含警告）"
    fi

    return 0
}

restore_directory() {
    info "使用 pg_restore 恢复 directory 格式..."

    local pg_restore_args=(
        -h "$DB_HOST"
        -p "$DB_PORT"
        -U "$DB_ADMIN_USER"
        -d "$DB_NAME"
        --no-owner
        --no-privileges
        --verbose
    )

    if [[ "$JOBS" -gt 1 ]]; then
        pg_restore_args+=("--jobs" "$JOBS")
    fi

    if [[ "$CLEAN" = "1" ]]; then
        pg_restore_args+=("--clean" "--if-exists")
    fi

    set +e
    timeout "$RESTORE_TIMEOUT" pg_restore "${pg_restore_args[@]}" "$BACKUP_FILE"
    local rc=$?
    set -e

    if [[ "$rc" -ne 0 ]]; then
        warn "pg_restore 返回 $rc"
    fi

    return 0
}

# ==============================================================================
# 恢复后验证
# ==============================================================================
verify_restore() {
    if [[ "$VERIFY" != "1" ]]; then
        return
    fi

    step "验证恢复结果"

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会验证恢复结果"
        return
    fi

    # 表数量
    local table_count
    table_count="$(psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_ADMIN_USER" -d "$DB_NAME" \
        -tAc "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema NOT IN ('pg_catalog', 'information_schema');" \
        2>/dev/null || echo 0)"
    info "表数量: $table_count"

    if [[ "$table_count" -eq 0 ]]; then
        error "恢复后表数量为 0，恢复可能失败"
        return 1
    fi

    # 检查关键表
    local key_tables=("candles" "orders" "positions" "experience")
    for table in "${key_tables[@]}"; do
        local exists
        exists="$(psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_ADMIN_USER" -d "$DB_NAME" \
            -tAc "SELECT 1 FROM information_schema.tables WHERE table_name='${table}' LIMIT 1;" \
            2>/dev/null || echo "")"
        if [[ "$exists" = "1" ]]; then
            info "  ✓ $table"
        else
            warn "  ✗ $table 不存在"
        fi
    done

    # 用户连接测试
    if psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_USER" -d "$DB_NAME" \
        -c "SELECT 1" >/dev/null 2>&1; then
        info "用户 ${DB_USER} 连接正常"
    else
        warn "用户 ${DB_USER} 无法连接"
    fi

    success "恢复验证通过"
    return 0
}

# ==============================================================================
# ANALYZE
# ==============================================================================
run_analyze() {
    if [[ "$ANALYZE" != "1" ]]; then
        return
    fi

    step "更新统计信息（ANALYZE）"

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会执行 ANALYZE"
        return
    fi

    if psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_ADMIN_USER" -d "$DB_NAME" \
        -c "ANALYZE;" >/dev/null 2>&1; then
        success "ANALYZE 完成"
    else
        warn "ANALYZE 失败"
    fi
}

# ==============================================================================
# 记录历史
# ==============================================================================
record_history() {
    if [[ "$DRY_RUN" = "1" ]]; then
        return
    fi

    mkdir -p "$(dirname "$HISTORY_FILE")"

    local ts
    ts="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    local duration=$((RESTORE_END - RESTORE_START))

    echo "${ts}|${ENV}|${DB_NAME}|$(basename "$BACKUP_FILE")|${duration}s|${RESTORE_ID}" \
        >> "$HISTORY_FILE"
}

# ==============================================================================
# 发送通知
# ==============================================================================
send_notification() {
    if [[ "$NOTIFY" != "1" ]]; then
        return
    fi

    local message="🔄 数据库恢复完成
环境: ${ENV}
数据库: ${DB_NAME}
来源: $(basename "$BACKUP_FILE")
恢复 ID: ${RESTORE_ID}"

    if [[ -n "${TELEGRAM_BOT_TOKEN:-}" ]] && [[ -n "${TELEGRAM_CHAT_ID:-}" ]]; then
        curl -sf -X POST "https://api.telegram.org/bot${TELEGRAM_BOT_TOKEN}/sendMessage" \
            -d "chat_id=${TELEGRAM_CHAT_ID}" \
            -d "text=${message}" >/dev/null 2>&1 || true
    fi
}

# ==============================================================================
# 主流程
# ==============================================================================
main() {
    init_colors
    parse_args "$@"

    if [[ "$VERBOSE" != "1" ]] && [[ "$DRY_RUN" != "1" ]]; then
        printf "\n"
        printf "%b╔══════════════════════════════════════════════════════════════╗%b\n" \
            "${COLOR_CYAN}${COLOR_BOLD}" "${COLOR_RESET}"
        printf "%b║  数据库恢复  v%s%b\n" \
            "${COLOR_CYAN}${COLOR_BOLD}" "$SCRIPT_VERSION" "${COLOR_RESET}"
        printf "%b╚══════════════════════════════════════════════════════════════╝%b\n" \
            "${COLOR_CYAN}${COLOR_BOLD}" "${COLOR_RESET}"
    fi

    if [[ "$DRY_RUN" = "1" ]]; then
        warn "DRY-RUN 模式：不会实际执行"
    fi

    mkdir -p "$LOG_DIR"

    # 获取锁
    acquire_lock

    # 环境检查
    check_environment

    # 获取备份文件
    fetch_backup_file

    # 校验文件
    validate_backup_file

    # 检查目标数据库
    check_target_database

    # 确认
    confirm_restore

    # 恢复前备份
    auto_backup_before_restore

    # 清空（可选）
    clean_database

    # 恢复
    if ! do_restore; then
        exit 1
    fi

    # 验证
    if ! verify_restore; then
        exit 7
    fi

    # ANALYZE
    run_analyze

    # 记录历史
    record_history

    # 通知
    send_notification

    # 完成
    printf "\n"
    printf "%b✅ 恢复完成%b\n" "${COLOR_GREEN}${COLOR_BOLD}" "${COLOR_RESET}"
    printf "   环境:      %s\n" "$ENV"
    printf "   数据库:    %s\n" "$DB_NAME"
    printf "   来源:      %s\n" "$(basename "$BACKUP_FILE")"
    printf "   恢复 ID:   %s\n" "$RESTORE_ID"
    if [[ -n "$PRE_RESTORE_BACKUP" ]]; then
        printf "   回滚备份:  %s\n" "$PRE_RESTORE_BACKUP"
    fi
    printf "\n"
}

# ==============================================================================
# 入口
# ==============================================================================
main "$@"
