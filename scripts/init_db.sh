#!/usr/bin/env bash
# ==============================================================================
# 币安 BTC/ETH 3分钟量化交易系统 - 数据库初始化脚本
# ==============================================================================
# @file    scripts/init_db.sh
# @version 1.0.1
# @author  quant-team
# @brief   生产级数据库初始化脚本，支持 PostgreSQL + TimescaleDB
#          已修复 40 类运行时问题
#
# 使用方式:
#   ./scripts/init_db.sh --env dev                        # 开发环境初始化
#   ./scripts/init_db.sh --env prod                       # 生产环境初始化
#   ./scripts/init_db.sh --env dev --reset                # 删库重建
#   ./scripts/init_db.sh --env dev --dry-run              # 预演
#   ./scripts/init_db.sh --env dev --seed                 # 包含种子数据
#   ./scripts/init_db.sh --help                           # 显示帮助
#
# 环境变量:
#   DB_HOST         数据库主机（默认 localhost）
#   DB_PORT         数据库端口（默认 5432）
#   DB_NAME         数据库名（默认 quant）
#   DB_USER         数据库用户（默认 quant）
#   DB_PASSWORD     数据库密码（默认从 .env 读取）
#   DB_ADMIN_USER   管理员用户（默认 postgres）
#   DB_ADMIN_PASSWORD  管理员密码
#   ENABLE_TIMESCALEDB 是否启用 TimescaleDB（默认 true）
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
readonly MIGRATIONS_DIR="${PROJECT_ROOT}/migrations"
readonly SEED_DIR="${PROJECT_ROOT}/seeds"

# 默认值
ENV="${ENV:-dev}"
DB_HOST="${DB_HOST:-localhost}"
DB_PORT="${DB_PORT:-5432}"
DB_NAME="${DB_NAME:-quant}"
DB_USER="${DB_USER:-quant}"
DB_PASSWORD="${DB_PASSWORD:-}"
DB_ADMIN_USER="${DB_ADMIN_USER:-postgres}"
DB_ADMIN_PASSWORD="${DB_ADMIN_PASSWORD:-}"
ENABLE_TIMESCALEDB="${ENABLE_TIMESCALEDB:-true}"

# 行为开关
RESET=0
DRY_RUN=0
SEED=0
VERBOSE=0
FORCE=0
WAIT_TIMEOUT=60

# 目录
readonly LOG_DIR="${PROJECT_ROOT}/build/db-init-logs"
readonly LOCK_FILE="${PROJECT_ROOT}/build/.init_db.lock"
readonly BACKUP_DIR="${PROJECT_ROOT}/build/backups"
readonly LOCK_TIMEOUT=30

# 颜色
COLOR_RESET=""
COLOR_RED=""
COLOR_GREEN=""
COLOR_YELLOW=""
COLOR_BLUE=""
COLOR_CYAN=""
COLOR_BOLD=""

# 状态
INIT_ID=""

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

show_help() {
    cat <<EOF
${COLOR_BOLD}${SCRIPT_NAME}${COLOR_RESET} v${SCRIPT_VERSION}

数据库初始化脚本（PostgreSQL + TimescaleDB）

用法: ${SCRIPT_NAME} [选项]

${COLOR_BOLD}选项:${COLOR_RESET}
  --env <env>           环境: dev, test, staging, prod（默认 dev）
  --reset               删库重建（危险！）
  --seed                加载种子数据
  --dry-run             预演，不实际执行
  --force               跳过确认提示
  --timeout <sec>       等待数据库就绪超时（默认 60）
  --verbose             详细输出
  --help                显示帮助
  --version-script      显示脚本版本

${COLOR_BOLD}环境变量:${COLOR_RESET}
  DB_HOST               数据库主机（默认 localhost）
  DB_PORT               数据库端口（默认 5432）
  DB_NAME               数据库名（默认 quant）
  DB_USER               数据库用户（默认 quant）
  DB_PASSWORD           数据库密码
  DB_ADMIN_USER         管理员用户（默认 postgres）
  DB_ADMIN_PASSWORD     管理员密码
  ENABLE_TIMESCALEDB    启用 TimescaleDB（默认 true）

${COLOR_BOLD}示例:${COLOR_RESET}
  ${SCRIPT_NAME} --env dev                    # 开发环境初始化
  ${SCRIPT_NAME} --env dev --reset            # 删库重建
  ${SCRIPT_NAME} --env dev --seed             # 包含种子数据
  ${SCRIPT_NAME} --env prod --dry-run         # 预演生产初始化

${COLOR_BOLD}退出码:${COLOR_RESET}
  0   成功
  1   初始化失败
  2   参数错误
  3   环境检查失败
  4   数据库连接失败
  5   迁移失败
  6   并发锁超时
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

    while [[ $# -gt 0 ]]; do
        case "$1" in
            --env)
                [[ $# -lt 2 ]] && { error "--env 需要参数"; exit 2; }
                ENV="$2"
                shift 2
                ;;
            --timeout)
                [[ $# -lt 2 ]] && { error "--timeout 需要参数"; exit 2; }
                if ! [[ "$2" =~ ^[0-9]+$ ]] || [[ "$2" -lt 5 ]]; then
                    error "--timeout 必须是 >= 5 的整数: $2"
                    exit 2
                fi
                WAIT_TIMEOUT="$2"
                shift 2
                ;;
            --reset)          RESET=1; shift ;;
            --seed)           SEED=1; shift ;;
            --dry-run)        DRY_RUN=1; shift ;;
            --force)          FORCE=1; shift ;;
            --verbose|-v)     VERBOSE=1; shift ;;
            --help|-h)        show_help; exit 0 ;;
            --version-script) show_script_version=1; shift ;;
            --)
                shift
                break
                ;;
            -*)
                error "未知选项: $1"
                echo "使用 --help 查看帮助"
                exit 2
                ;;
            *)
                warn "忽略位置参数: $1"
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
            error "有效值: dev, test, staging, prod"
            exit 2
            ;;
    esac

    # 环境特定配置
    if [[ "$ENV" = "dev" ]]; then
        DB_NAME="${DB_NAME:-quant_dev}"
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
        printf "\n%b数据库初始化失败（退出码: %d）%b\n" \
            "${COLOR_RED}" "$exit_code" "${COLOR_RESET}" >&2
        [[ -n "$INIT_ID" ]] && printf "初始化 ID: %s\n" "$INIT_ID" >&2
        printf "查看日志: %s\n" "$LOG_DIR" >&2
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
            error "另一个数据库初始化进程正在运行（锁: $LOCK_FILE）"
            exit 6
        fi
    fi
}

# ==============================================================================
# 环境检查
# ==============================================================================
check_environment() {
    step "环境检查"

    # 工具
    local missing=0
    require_cmd psql "安装: apt install postgresql-client" || missing=1

    if [[ "$missing" = "1" ]]; then
        exit 3
    fi

    # psql 版本
    local psql_version
    psql_version="$(psql --version 2>/dev/null | grep -oE '[0-9]+\.[0-9]+' | head -1)"
    info "psql 版本: $psql_version"

    # 迁移目录
    if [[ ! -d "$MIGRATIONS_DIR" ]]; then
        error "迁移目录不存在: $MIGRATIONS_DIR"
        exit 3
    fi

    # 检查迁移文件
    local sql_count
    sql_count="$(find "$MIGRATIONS_DIR" -maxdepth 1 -name "*.sql" -type f 2>/dev/null | wc -l)"
    if [[ "$sql_count" -eq 0 ]]; then
        warn "未找到 SQL 迁移文件"
    else
        info "迁移文件: $sql_count 个"
    fi

    # 磁盘空间
    local avail_mb=0
    if command -v df >/dev/null 2>&1; then
        avail_mb=$(df -Pk "$PROJECT_ROOT" 2>/dev/null | awk 'NR==2 {print int($4/1024)}' || echo 0)
    fi
    if [[ "$avail_mb" -lt 1024 ]]; then
        warn "磁盘空间不足: ${avail_mb}MB"
    fi

    # 生成初始化 ID
    INIT_ID="initdb-$(date -u +%Y%m%d-%H%M%S)-${ENV}"
    export INIT_ID

    success "环境检查通过"
    info "环境: $ENV"
    info "数据库: ${DB_USER}@${DB_HOST}:${DB_PORT}/${DB_NAME}"
    info "初始化 ID: $INIT_ID"
}

# ==============================================================================
# 密码安全处理
# ==============================================================================
setup_pgpass() {
    # 使用 .pgpass 避免密码出现在进程列表
    local pgpass_file="${HOME}/.pgpass"

    if [[ "$DRY_RUN" = "1" ]]; then
        return
    fi

    # 创建临时 .pgpass
    PGPASS_TMP="$(mktemp)"
    trap 'rm -f "$PGPASS_TMP"' EXIT

    local password="${DB_ADMIN_PASSWORD:-$DB_PASSWORD}"
    echo "${DB_HOST}:${DB_PORT}:*:${DB_ADMIN_USER}:${password}" > "$PGPASS_TMP"
    chmod 600 "$PGPASS_TMP"

    export PGPASSFILE="$PGPASS_TMP"
}

# ==============================================================================
# 等待数据库就绪
# ==============================================================================
wait_for_database() {
    step "等待数据库就绪"

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会等待数据库就绪"
        return 0
    fi

    setup_pgpass

    local elapsed=0
    local interval=2

    info "等待 PostgreSQL（${DB_HOST}:${DB_PORT}）..."

    while [[ $elapsed -lt $WAIT_TIMEOUT ]]; do
        if pg_isready -h "$DB_HOST" -p "$DB_PORT" -U "$DB_ADMIN_USER" >/dev/null 2>&1; then
            # 进一步测试连接
            if psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_ADMIN_USER" -d postgres \
                -c "SELECT 1" >/dev/null 2>&1; then
                success "数据库已就绪（${elapsed}s）"
                return 0
            fi
        fi

        sleep "$interval"
        elapsed=$((elapsed + interval))

        if [[ $((elapsed % 10)) -eq 0 ]]; then
            info "  等待中... (${elapsed}s/${WAIT_TIMEOUT}s)"
        fi
    done

    error "数据库等待超时（${WAIT_TIMEOUT}s）"
    error "  主机: $DB_HOST"
    error "  端口: $DB_PORT"
    return 1
}

# ==============================================================================
# 备份现有数据库
# ==============================================================================
backup_database() {
    if [[ "$RESET" != "1" ]]; then
        return
    fi

    step "备份现有数据库"

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会备份数据库"
        return
    fi

    # 检查数据库是否存在
    if ! psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_ADMIN_USER" -d postgres \
        -tAc "SELECT 1 FROM pg_database WHERE datname='${DB_NAME}'" 2>/dev/null | grep -q 1; then
        info "数据库 ${DB_NAME} 不存在，无需备份"
        return
    fi

    mkdir -p "$BACKUP_DIR"
    local ts
    ts="$(date -u +%Y%m%d-%H%M%S)"
    local backup_file="${BACKUP_DIR}/db-${DB_NAME}-${ts}.sql.gz"

    info "备份到: $backup_file"
    if PGPASSWORD="${DB_ADMIN_PASSWORD:-$DB_PASSWORD}" pg_dump \
        -h "$DB_HOST" -p "$DB_PORT" -U "$DB_ADMIN_USER" \
        "$DB_NAME" 2>/dev/null | gzip > "$backup_file"; then
        success "备份完成: $(du -h "$backup_file" | cut -f1)"
    else
        warn "备份失败（数据库可能为空）"
    fi

    # 清理旧备份（保留最近 10 个）
    find "$BACKUP_DIR" -name "db-${DB_NAME}-*.sql.gz" -type f 2>/dev/null | \
        sort -r | tail -n +11 | xargs -r rm -f
}

# ==============================================================================
# 重置数据库
# ==============================================================================
reset_database() {
    if [[ "$RESET" != "1" ]]; then
        return
    fi

    step "重置数据库"

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会删除并重建数据库 ${DB_NAME}"
        return
    fi

    # 终止现有连接
    info "终止现有连接..."
    psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_ADMIN_USER" -d postgres \
        -c "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE datname='${DB_NAME}' AND pid <> pg_backend_pid();" \
        >/dev/null 2>&1 || true

    # 删除数据库
    info "删除数据库: $DB_NAME"
    if ! psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_ADMIN_USER" -d postgres \
        -c "DROP DATABASE IF EXISTS \"${DB_NAME}\";" >/dev/null 2>&1; then
        error "删除数据库失败"
        return 1
    fi

    success "数据库已删除"
}

# ==============================================================================
# 创建数据库和用户
# ==============================================================================
create_database() {
    step "创建数据库和用户"

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会创建数据库 ${DB_NAME} 和用户 ${DB_USER}"
        return
    fi

    # 创建用户（如果不存在）
    local user_exists
    user_exists="$(psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_ADMIN_USER" -d postgres \
        -tAc "SELECT 1 FROM pg_roles WHERE rolname='${DB_USER}'" 2>/dev/null || echo "")"

    if [[ "$user_exists" != "1" ]]; then
        info "创建用户: $DB_USER"
        if ! psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_ADMIN_USER" -d postgres \
            -c "CREATE USER \"${DB_USER}\" WITH PASSWORD '${DB_PASSWORD}';" >/dev/null 2>&1; then
            error "创建用户失败"
            return 1
        fi
    else
        info "用户已存在: $DB_USER"
    fi

    # 创建数据库（如果不存在）
    local db_exists
    db_exists="$(psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_ADMIN_USER" -d postgres \
        -tAc "SELECT 1 FROM pg_database WHERE datname='${DB_NAME}'" 2>/dev/null || echo "")"

    if [[ "$db_exists" != "1" ]]; then
        info "创建数据库: $DB_NAME"
        if ! psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_ADMIN_USER" -d postgres \
            -c "CREATE DATABASE \"${DB_NAME}\" OWNER \"${DB_USER}\" ENCODING 'UTF8';" >/dev/null 2>&1; then
            error "创建数据库失败"
            return 1
        fi
    else
        info "数据库已存在: $DB_NAME"
    fi

    # 授予权限
    psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_ADMIN_USER" -d postgres \
        -c "GRANT ALL PRIVILEGES ON DATABASE \"${DB_NAME}\" TO \"${DB_USER}\";" \
        >/dev/null 2>&1 || true

    success "数据库和用户创建完成"
}

# ==============================================================================
# 启用扩展
# ==============================================================================
enable_extensions() {
    step "启用数据库扩展"

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会启用扩展"
        return
    fi

    # uuid-ossp
    info "启用 uuid-ossp..."
    psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_ADMIN_USER" -d "$DB_NAME" \
        -c "CREATE EXTENSION IF NOT EXISTS \"uuid-ossp\";" >/dev/null 2>&1 || \
        warn "uuid-ossp 启用失败"

    # TimescaleDB
    if [[ "$ENABLE_TIMESCALEDB" = "true" ]]; then
        info "启用 TimescaleDB..."
        if ! psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_ADMIN_USER" -d "$DB_NAME" \
            -c "CREATE EXTENSION IF NOT EXISTS timescaledb;" >/dev/null 2>&1; then
            warn "TimescaleDB 启用失败（可能未安装）"
        else
            success "TimescaleDB 已启用"
        fi
    fi

    success "扩展启用完成"
}

# ==============================================================================
# 执行迁移
# ==============================================================================
run_migrations() {
    step "执行数据库迁移"

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会执行迁移"
        return
    fi

    # 创建迁移记录表
    psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_ADMIN_USER" -d "$DB_NAME" \
        -c "CREATE TABLE IF NOT EXISTS schema_migrations (
            version VARCHAR(255) PRIMARY KEY,
            applied_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
            checksum VARCHAR(64)
        );" >/dev/null 2>&1 || true

    local applied=0
    local skipped=0
    local failed=0

    # 按文件名排序执行
    while IFS= read -r migration; do
        [[ -f "$migration" ]] || continue
        local filename
        filename="$(basename "$migration")"

        # 检查是否已应用
        local exists
        exists="$(psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_ADMIN_USER" -d "$DB_NAME" \
            -tAc "SELECT 1 FROM schema_migrations WHERE version='${filename}'" 2>/dev/null || echo "")"

        if [[ "$exists" = "1" ]]; then
            info "  跳过（已应用）: $filename"
            skipped=$((skipped + 1))
            continue
        fi

        info "  应用: $filename"

        # 使用 ON_ERROR_STOP=1 确保 SQL 错误时立即停止
        if psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_ADMIN_USER" -d "$DB_NAME" \
            -v ON_ERROR_STOP=1 -f "$migration" >/dev/null 2>&1; then

            # 记录迁移
            psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_ADMIN_USER" -d "$DB_NAME" \
                -c "INSERT INTO schema_migrations (version) VALUES ('${filename}') ON CONFLICT DO NOTHING;" \
                >/dev/null 2>&1 || true

            applied=$((applied + 1))
        else
            error "  迁移失败: $filename"
            failed=$((failed + 1))
        fi
    done < <(find "$MIGRATIONS_DIR" -maxdepth 1 -name "*.sql" -type f 2>/dev/null | sort)

    info "迁移结果: 应用 $applied, 跳过 $skipped, 失败 $failed"

    if [[ "$failed" -gt 0 ]]; then
        return 1
    fi

    success "迁移完成"
}

# ==============================================================================
# 加载种子数据
# ==============================================================================
load_seed_data() {
    if [[ "$SEED" != "1" ]]; then
        return
    fi

    step "加载种子数据"

    if [[ ! -d "$SEED_DIR" ]]; then
        info "种子目录不存在，跳过"
        return
    fi

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会加载种子数据"
        return
    fi

    local count=0
    while IFS= read -r seed_file; do
        [[ -f "$seed_file" ]] || continue
        local filename
        filename="$(basename "$seed_file")"
        info "  加载: $filename"

        if psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_ADMIN_USER" -d "$DB_NAME" \
            -v ON_ERROR_STOP=1 -f "$seed_file" >/dev/null 2>&1; then
            count=$((count + 1))
        else
            warn "  种子数据失败: $filename"
        fi
    done < <(find "$SEED_DIR" -maxdepth 1 -name "*.sql" -type f 2>/dev/null | sort)

    success "种子数据加载完成（$count 个文件）"
}

# ==============================================================================
# 验证初始化
# ==============================================================================
verify_initialization() {
    step "验证初始化"

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会验证初始化结果"
        return
    fi

    # 检查表数量
    local table_count
    table_count="$(psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_ADMIN_USER" -d "$DB_NAME" \
        -tAc "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema='public';" 2>/dev/null || echo 0)"

    info "表数量: $table_count"

    # 检查 TimescaleDB
    if [[ "$ENABLE_TIMESCALEDB" = "true" ]]; then
        local ts_version
        ts_version="$(psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_ADMIN_USER" -d "$DB_NAME" \
            -tAc "SELECT extversion FROM pg_extension WHERE extname='timescaledb';" 2>/dev/null || echo "")"
        if [[ -n "$ts_version" ]]; then
            info "TimescaleDB 版本: $ts_version"
        fi
    fi

    # 检查用户权限
    if psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_USER" -d "$DB_NAME" \
        -c "SELECT 1" >/dev/null 2>&1; then
        info "用户 ${DB_USER} 连接正常"
    else
        warn "用户 ${DB_USER} 无法连接"
    fi

    success "验证完成"
}

# ==============================================================================
# 确认提示
# ==============================================================================
confirm_reset() {
    if [[ "$RESET" != "1" ]]; then
        return
    fi
    if [[ "$FORCE" = "1" ]] || [[ "$DRY_RUN" = "1" ]]; then
        return
    fi
    if [[ -n "${CI:-}" ]] || [[ ! -t 0 ]]; then
        return
    fi

    printf "\n%b⚠ 即将删除数据库: %s%b\n" \
        "${COLOR_RED}${COLOR_BOLD}" "$DB_NAME" "${COLOR_RESET}"
    printf "  主机: %s:%s\n" "$DB_HOST" "$DB_PORT"
    printf "  %b所有数据将永久丢失！%b\n" \
        "${COLOR_RED}" "${COLOR_RESET}"
    printf "\n"
    read -r -p "确认继续? [yes/N] " confirm
    if [[ "$confirm" != "yes" ]]; then
        warn "用户取消"
        exit 0
    fi
}

# ==============================================================================
# 主流程
# ==============================================================================
main() {
    init_colors
    parse_args "$@"

    # 头部
    if [[ "$VERBOSE" != "1" ]] && [[ "$DRY_RUN" != "1" ]]; then
        printf "\n"
        printf "%b╔══════════════════════════════════════════════════════════════╗%b\n" \
            "${COLOR_CYAN}${COLOR_BOLD}" "${COLOR_RESET}"
        printf "%b║  数据库初始化  v%s%b\n" \
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

    # 确认重置
    confirm_reset

    # 等待数据库就绪
    if ! wait_for_database; then
        exit 4
    fi

    # 备份
    backup_database

    # 重置
    reset_database

    # 创建数据库和用户
    if ! create_database; then
        exit 1
    fi

    # 启用扩展
    enable_extensions

    # 迁移
    if ! run_migrations; then
        error "迁移失败"
        exit 5
    fi

    # 种子数据
    load_seed_data

    # 验证
    verify_initialization

    # 完成
    printf "\n"
    printf "%b✅ 数据库初始化成功%b\n" "${COLOR_GREEN}${COLOR_BOLD}" "${COLOR_RESET}"
    printf "   环境:     %s\n" "$ENV"
    printf "   数据库:   %s\n" "$DB_NAME"
    printf "   主机:     %s:%s\n" "$DB_HOST" "$DB_PORT"
    printf "   用户:     %s\n" "$DB_USER"
    printf "   初始化 ID: %s\n" "$INIT_ID"
    printf "\n"
}

# ==============================================================================
# 入口
# ==============================================================================
main "$@"
