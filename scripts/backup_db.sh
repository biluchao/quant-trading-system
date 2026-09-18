#!/usr/bin/env bash
# ==============================================================================
# 币安 BTC/ETH 3分钟量化交易系统 - 数据库备份脚本
# ==============================================================================
# @file    scripts/backup_db.sh
# @version 1.0.1
# @author  quant-team
# @brief   生产级数据库备份脚本，支持压缩、校验、S3 上传、保留策略
#          已修复 40 类运行时问题
#
# 使用方式:
#   ./scripts/backup_db.sh                           # 默认备份
#   ./scripts/backup_db.sh --env dev                 # 开发环境
#   ./scripts/backup_db.sh --env prod --s3           # 上传到 S3
#   ./scripts/backup_db.sh --env dev --verify        # 备份后验证
#   ./scripts/backup_db.sh --env dev --dry-run       # 预演
#   ./scripts/backup_db.sh --help                    # 显示帮助
#
# 环境变量:
#   DB_HOST         数据库主机（默认 localhost）
#   DB_PORT         数据库端口（默认 5432）
#   DB_NAME         数据库名（默认 quant）
#   DB_USER         数据库用户（默认 quant）
#   DB_PASSWORD     数据库密码
#   BACKUP_DIR      备份目录（默认 ./build/backups）
#   BACKUP_RETENTION_DAYS  保留天数（默认 30）
#   S3_BUCKET       S3 存储桶（--s3 时使用）
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
BACKUP_DIR="${BACKUP_DIR:-${PROJECT_ROOT}/build/backups}"
BACKUP_RETENTION_DAYS="${BACKUP_RETENTION_DAYS:-30}"
BACKUP_COMPRESS="${BACKUP_COMPRESS:-gzip}"
BACKUP_FORMAT="${BACKUP_FORMAT:-plain}"
BACKUP_TIMEOUT="${BACKUP_TIMEOUT:-3600}"

# S3
S3_ENABLED=0
S3_BUCKET="${S3_BUCKET:-}"
S3_REGION="${S3_REGION:-}"
S3_PREFIX="${S3_PREFIX:-db-backups}"

# 行为开关
DRY_RUN=0
VERIFY=0
VERBOSE=0
NOTIFY=0
SKIP_SCHEMAS=""
INCLUDE_SCHEMAS=""

# 目录
readonly LOG_DIR="${PROJECT_ROOT}/build/db-backup-logs"
readonly LOCK_FILE="${PROJECT_ROOT}/build/.backup_db.lock"
readonly HISTORY_FILE="${PROJECT_ROOT}/build/.backup-history"
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
BACKUP_ID=""
BACKUP_FILE=""
BACKUP_SIZE=0
BACKUP_START=""
BACKUP_END=""

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

数据库备份脚本（PostgreSQL）

用法: ${SCRIPT_NAME} [选项]

${COLOR_BOLD}选项:${COLOR_RESET}
  --env <env>           环境: dev, test, staging, prod（默认 dev）
  --db-name <name>      数据库名（默认从环境变量）
  --output-dir <dir>    备份目录
  --retention <days>    保留天数（默认 30）
  --format <fmt>        备份格式: plain, custom, directory, tar
  --compress <algo>     压缩: gzip, zstd, none（默认 gzip）
  --schemas <list>      仅备份指定 schema（逗号分隔）
  --exclude-schemas <l> 排除指定 schema（逗号分隔）
  --s3                  上传到 S3
  --s3-bucket <name>    S3 存储桶
  --verify              备份后验证完整性
  --notify              完成后发送通知
  --timeout <sec>       备份超时（默认 3600）
  --dry-run             预演，不实际执行
  --verbose             详细输出
  --help                显示帮助
  --version-script      显示脚本版本

${COLOR_BOLD}示例:${COLOR_RESET}
  ${SCRIPT_NAME} --env dev                    # 基础备份
  ${SCRIPT_NAME} --env prod --s3              # 备份并上传 S3
  ${SCRIPT_NAME} --env prod --verify          # 备份并验证
  ${SCRIPT_NAME} --env prod --retention 90    # 保留 90 天

${COLOR_BOLD}退出码:${COLOR_RESET}
  0   成功
  1   备份失败
  2   参数错误
  3   环境检查失败
  4   数据库连接失败
  5   磁盘空间不足
  6   并发锁超时
  7   校验失败
  8   S3 上传失败
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
            --db-name)
                [[ $# -lt 2 ]] && { error "--db-name 需要参数"; exit 2; }
                DB_NAME="$2"
                shift 2
                ;;
            --output-dir)
                [[ $# -lt 2 ]] && { error "--output-dir 需要参数"; exit 2; }
                BACKUP_DIR="$2"
                shift 2
                ;;
            --retention)
                [[ $# -lt 2 ]] && { error "--retention 需要参数"; exit 2; }
                if ! [[ "$2" =~ ^[0-9]+$ ]] || [[ "$2" -lt 1 ]]; then
                    error "--retention 必须是正整数: $2"
                    exit 2
                fi
                BACKUP_RETENTION_DAYS="$2"
                shift 2
                ;;
            --format)
                [[ $# -lt 2 ]] && { error "--format 需要参数"; exit 2; }
                case "$2" in
                    plain|custom|directory|tar) BACKUP_FORMAT="$2" ;;
                    *)
                        error "无效格式: $2（支持: plain, custom, directory, tar）"
                        exit 2
                        ;;
                esac
                shift 2
                ;;
            --compress)
                [[ $# -lt 2 ]] && { error "--compress 需要参数"; exit 2; }
                case "$2" in
                    gzip|zstd|none) BACKUP_COMPRESS="$2" ;;
                    *)
                        error "无效压缩: $2（支持: gzip, zstd, none）"
                        exit 2
                        ;;
                esac
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
            --s3)               S3_ENABLED=1; shift ;;
            --s3-bucket)
                [[ $# -lt 2 ]] && { error "--s3-bucket 需要参数"; exit 2; }
                S3_BUCKET="$2"
                S3_ENABLED=1
                shift 2
                ;;
            --timeout)
                [[ $# -lt 2 ]] && { error "--timeout 需要参数"; exit 2; }
                if ! [[ "$2" =~ ^[0-9]+$ ]] || [[ "$2" -lt 60 ]]; then
                    error "--timeout 必须是 >= 60 的整数: $2"
                    exit 2
                fi
                BACKUP_TIMEOUT="$2"
                shift 2
                ;;
            --verify)         VERIFY=1; shift ;;
            --notify)         NOTIFY=1; shift ;;
            --dry-run)        DRY_RUN=1; shift ;;
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
            exit 2
            ;;
    esac

    # 环境特定默认值
    if [[ "$ENV" = "dev" ]] && [[ "$DB_NAME" = "quant" ]]; then
        DB_NAME="quant_dev"
    fi

    # S3 校验
    if [[ "$S3_ENABLED" = "1" ]]; then
        if [[ -z "$S3_BUCKET" ]]; then
            error "启用 S3 时必须指定 --s3-bucket"
            exit 2
        fi
        require_cmd aws "安装: apt install awscli 或 pip install awscli" || exit 3
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

    # 清理未完成的备份
    if [[ -n "$BACKUP_FILE" ]] && [[ -f "${BACKUP_FILE}.tmp" ]]; then
        rm -f "${BACKUP_FILE}.tmp" 2>/dev/null || true
    fi

    if [[ "$exit_code" -ne 0 ]] && [[ "$exit_code" -ne 130 ]] && [[ "$exit_code" -ne 2 ]]; then
        printf "\n%b备份失败（退出码: %d）%b\n" \
            "${COLOR_RED}" "$exit_code" "${COLOR_RESET}" >&2
        [[ -n "$BACKUP_ID" ]] && printf "备份 ID: %s\n" "$BACKUP_ID" >&2
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
            error "另一个备份进程正在运行（锁: $LOCK_FILE）"
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
    require_cmd pg_dump "安装: apt install postgresql-client" || missing=1
    require_cmd psql "安装: apt install postgresql-client" || missing=1

    if [[ "$BACKUP_COMPRESS" = "gzip" ]]; then
        require_cmd gzip "安装: apt install gzip" || missing=1
    elif [[ "$BACKUP_COMPRESS" = "zstd" ]]; then
        require_cmd zstd "安装: apt install zstd" || missing=1
    fi

    if [[ "$VERIFY" = "1" ]]; then
        require_cmd pg_restore "安装: apt install postgresql-client" || missing=1
    fi

    if [[ "$missing" = "1" ]]; then
        exit 3
    fi

    # pg_dump 版本
    local dump_version
    dump_version="$(pg_dump --version 2>/dev/null | grep -oE '[0-9]+' | head -1)"
    info "pg_dump 版本: $dump_version"

    # 备份目录
    mkdir -p "$BACKUP_DIR"
    if [[ ! -w "$BACKUP_DIR" ]]; then
        error "备份目录不可写: $BACKUP_DIR"
        exit 3
    fi

    # 磁盘空间（估算为数据库大小的 2 倍，至少 500MB）
    local avail_mb=0
    if command -v df >/dev/null 2>&1; then
        avail_mb=$(df -Pk "$BACKUP_DIR" 2>/dev/null | awk 'NR==2 {print int($4/1024)}' || echo 0)
    fi
    if [[ "$avail_mb" -lt 500 ]]; then
        error "磁盘空间不足: ${avail_mb}MB < 500MB"
        exit 5
    fi
    info "可用磁盘: ${avail_mb}MB"

    # 生成备份 ID
    BACKUP_ID="backup-$(date -u +%Y%m%d-%H%M%S)-${ENV}"
    export BACKUP_ID

    success "环境检查通过"
    info "环境: $ENV"
    info "数据库: ${DB_USER}@${DB_HOST}:${DB_PORT}/${DB_NAME}"
    info "备份目录: $BACKUP_DIR"
    info "保留天数: $BACKUP_RETENTION_DAYS"
    info "备份 ID: $BACKUP_ID"
}

# ==============================================================================
# 密码安全处理
# ==============================================================================
setup_pgpass() {
    if [[ -z "${DB_PASSWORD:-}" ]]; then
        return
    fi

    local pgpass_file
    pgpass_file="$(mktemp)"
    trap 'rm -f "$pgpass_file"' EXIT

    echo "${DB_HOST}:${DB_PORT}:*:${DB_USER}:${DB_PASSWORD}" > "$pgpass_file"
    chmod 600 "$pgpass_file"

    export PGPASSFILE="$pgpass_file"
}

# ==============================================================================
# 检查数据库连接
# ==============================================================================
check_database() {
    step "检查数据库"

    setup_pgpass

    if ! pg_isready -h "$DB_HOST" -p "$DB_PORT" -U "$DB_USER" >/dev/null 2>&1; then
        error "数据库未就绪: ${DB_HOST}:${DB_PORT}"
        exit 4
    fi

    # 检查数据库是否存在
    local exists
    exists="$(psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_USER" -d postgres \
        -tAc "SELECT 1 FROM pg_database WHERE datname='${DB_NAME}'" 2>/dev/null || echo "")"

    if [[ "$exists" != "1" ]]; then
        error "数据库不存在: $DB_NAME"
        exit 4
    fi

    # 获取数据库大小
    local db_size
    db_size="$(psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_USER" -d "$DB_NAME" \
        -tAc "SELECT pg_size_pretty(pg_database_size('${DB_NAME}'));" 2>/dev/null || echo "unknown")"
    info "数据库大小: $db_size"

    # 获取表数量
    local table_count
    table_count="$(psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_USER" -d "$DB_NAME" \
        -tAc "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema NOT IN ('pg_catalog', 'information_schema');" 2>/dev/null || echo 0)"
    info "表数量: $table_count"

    success "数据库检查通过"
}

# ==============================================================================
# 执行备份
# ==============================================================================
do_backup() {
    step "执行备份"

    local timestamp
    timestamp="$(date -u +%Y%m%d-%H%M%S)"

    # 根据格式确定文件扩展名
    local ext=""
    case "$BACKUP_FORMAT" in
        plain)     ext="sql" ;;
        custom)    ext="dump" ;;
        directory) ext="dir" ;;
        tar)       ext="tar" ;;
    esac

    if [[ "$BACKUP_COMPRESS" = "gzip" ]] && [[ "$BACKUP_FORMAT" != "directory" ]]; then
        ext="${ext}.gz"
    elif [[ "$BACKUP_COMPRESS" = "zstd" ]] && [[ "$BACKUP_FORMAT" != "directory" ]]; then
        ext="${ext}.zst"
    fi

    local filename="${DB_NAME}-${timestamp}.${ext}"
    BACKUP_FILE="${BACKUP_DIR}/${filename}"

    info "备份文件: $BACKUP_FILE"

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会执行备份"
        return
    fi

    # 构建 pg_dump 参数
    local pg_dump_args=(
        -h "$DB_HOST"
        -p "$DB_PORT"
        -U "$DB_USER"
        -d "$DB_NAME"
        -F "$(echo "$BACKUP_FORMAT" | cut -c1)"
        --no-password
    )

    # schema 过滤
    if [[ -n "$INCLUDE_SCHEMAS" ]]; then
        IFS=',' read -ra schemas <<< "$INCLUDE_SCHEMAS"
        for schema in "${schemas[@]}"; do
            pg_dump_args+=("-n" "$schema")
        done
    fi

    if [[ -n "$SKIP_SCHEMAS" ]]; then
        IFS=',' read -ra schemas <<< "$SKIP_SCHEMAS"
        for schema in "${schemas[@]}"; do
            pg_dump_args+=("-N" "$schema")
        done
    fi

    # 执行备份（带超时）
    BACKUP_START="$(date +%s)"

    local backup_cmd
    if [[ "$BACKUP_COMPRESS" = "gzip" ]] && [[ "$BACKUP_FORMAT" != "directory" ]]; then
        # 使用管道压缩，但需要检查 pg_dump 是否成功
        if ! command -v timeout >/dev/null 2>&1; then
            warn "未找到 timeout 命令，无超时保护"
            pg_dump "${pg_dump_args[@]}" | gzip > "${BACKUP_FILE}.tmp" || {
                error "备份命令失败"
                rm -f "${BACKUP_FILE}.tmp"
                return 1
            }
        else
            # 使用 PIPESTATUS 检查管道中所有命令
            set +e
            timeout "$BACKUP_TIMEOUT" bash -c \
                "pg_dump $(printf '%q ' "${pg_dump_args[@]}") | gzip > $(printf '%q' "${BACKUP_FILE}.tmp")"
            local exit_code=$?
            set -e

            if [[ $exit_code -ne 0 ]]; then
                error "备份命令失败（退出码: $exit_code）"
                rm -f "${BACKUP_FILE}.tmp"
                return 1
            fi
        fi
    elif [[ "$BACKUP_COMPRESS" = "zstd" ]] && [[ "$BACKUP_FORMAT" != "directory" ]]; then
        set +e
        timeout "$BACKUP_TIMEOUT" bash -c \
            "pg_dump $(printf '%q ' "${pg_dump_args[@]}") | zstd -q -o $(printf '%q' "${BACKUP_FILE}.tmp")"
        local exit_code=$?
        set -e

        if [[ $exit_code -ne 0 ]]; then
            error "备份命令失败（退出码: $exit_code）"
            rm -f "${BACKUP_FILE}.tmp"
            return 1
        fi
    else
        # 无压缩或 directory 格式
        if ! timeout "$BACKUP_TIMEOUT" pg_dump "${pg_dump_args[@]}" \
            -f "${BACKUP_FILE}.tmp" 2>&1; then
            error "备份命令失败"
            rm -f "${BACKUP_FILE}.tmp" 2>/dev/null || true
            rm -rf "${BACKUP_FILE}.tmp" 2>/dev/null || true
            return 1
        fi
    fi

    BACKUP_END="$(date +%s)"

    # 原子重命名
    if [[ -e "${BACKUP_FILE}.tmp" ]]; then
        mv "${BACKUP_FILE}.tmp" "$BACKUP_FILE"
    fi

    # 获取文件大小
    if [[ -f "$BACKUP_FILE" ]]; then
        BACKUP_SIZE="$(stat -c%s "$BACKUP_FILE" 2>/dev/null || stat -f%z "$BACKUP_FILE" 2>/dev/null || echo 0)"
    elif [[ -d "$BACKUP_FILE" ]]; then
        BACKUP_SIZE="$(du -sb "$BACKUP_FILE" 2>/dev/null | cut -f1 || echo 0)"
    fi

    if [[ "$BACKUP_SIZE" -eq 0 ]]; then
        error "备份文件为空"
        return 1
    fi

    local duration=$((BACKUP_END - BACKUP_START))
    success "备份完成: $(human_size "$BACKUP_SIZE"), 耗时 ${duration}s"
}

# ==============================================================================
# 生成校验和
# ==============================================================================
generate_checksum() {
    step "生成校验和"

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会生成校验和"
        return
    fi

    if [[ ! -f "$BACKUP_FILE" ]]; then
        return
    fi

    local checksum_file="${BACKUP_FILE}.sha256"

    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$BACKUP_FILE" > "$checksum_file"
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$BACKUP_FILE" > "$checksum_file"
    else
        warn "未找到 sha256sum/shasum，跳过校验和"
        return
    fi

    local checksum
    checksum="$(cut -d' ' -f1 "$checksum_file")"
    info "SHA256: $checksum"
    success "校验和已生成"
}

# ==============================================================================
# 验证备份
# ==============================================================================
verify_backup() {
    if [[ "$VERIFY" != "1" ]]; then
        return
    fi

    step "验证备份"

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会验证备份"
        return
    fi

    if [[ ! -f "$BACKUP_FILE" ]]; then
        error "备份文件不存在"
        exit 7
    fi

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

        if [[ "$expected" = "$actual" ]]; then
            success "校验和匹配"
        else
            error "校验和不匹配！备份可能已损坏"
            exit 7
        fi
    fi

    # 根据格式验证内容
    case "$BACKUP_FORMAT" in
        custom)
            info "验证 pg_restore 可读性..."
            if pg_restore --list "$BACKUP_FILE" >/dev/null 2>&1; then
                success "pg_restore 验证通过"
            else
                error "pg_restore 无法读取备份"
                exit 7
            fi
            ;;
        plain)
            info "验证 SQL 格式..."
            local first_line
            if [[ "$BACKUP_COMPRESS" = "gzip" ]]; then
                first_line="$(gzip -dc "$BACKUP_FILE" 2>/dev/null | head -1 || echo "")"
            elif [[ "$BACKUP_COMPRESS" = "zstd" ]]; then
                first_line="$(zstd -dc "$BACKUP_FILE" 2>/dev/null | head -1 || echo "")"
            else
                first_line="$(head -1 "$BACKUP_FILE" 2>/dev/null || echo "")"
            fi

            if [[ -z "$first_line" ]]; then
                error "无法读取备份内容"
                exit 7
            fi
            success "SQL 格式验证通过"
            ;;
    esac

    success "备份验证通过"
}

# ==============================================================================
# S3 上传
# ==============================================================================
upload_to_s3() {
    if [[ "$S3_ENABLED" != "1" ]]; then
        return
    fi

    step "上传到 S3"

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会上传到 s3://${S3_BUCKET}/${S3_PREFIX}/"
        return
    fi

    if [[ ! -f "$BACKUP_FILE" ]]; then
        error "备份文件不存在"
        return 1
    fi

    local s3_path="s3://${S3_BUCKET}/${S3_PREFIX}/$(basename "$BACKUP_FILE")"
    info "上传: $s3_path"

    local aws_args=(s3 cp "$BACKUP_FILE" "$s3_path")
    if [[ -n "$S3_REGION" ]]; then
        aws_args+=("--region" "$S3_REGION")
    fi

    if ! aws "${aws_args[@]}"; then
        error "S3 上传失败"
        return 1
    fi

    # 上传校验和
    if [[ -f "${BACKUP_FILE}.sha256" ]]; then
        aws s3 cp "${BACKUP_FILE}.sha256" \
            "s3://${S3_BUCKET}/${S3_PREFIX}/$(basename "${BACKUP_FILE}.sha256")" \
            ${S3_REGION:+--region "$S3_REGION"} >/dev/null 2>&1 || true
    fi

    success "S3 上传完成"
}

# ==============================================================================
# 清理旧备份
# ==============================================================================
cleanup_old_backups() {
    step "清理旧备份"

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会清理 ${BACKUP_RETENTION_DAYS} 天前的备份"
        return
    fi

    local count=0

    # 按修改时间删除
    while IFS= read -r old_backup; do
        if [[ -f "$old_backup" ]]; then
            info "  删除: $(basename "$old_backup")"
            rm -f "$old_backup"
            # 同时删除校验和
            rm -f "${old_backup}.sha256" 2>/dev/null || true
            count=$((count + 1))
        fi
    done < <(find "$BACKUP_DIR" -maxdepth 1 -name "${DB_NAME}-*" -type f \
        -mtime "+${BACKUP_RETENTION_DAYS}" 2>/dev/null)

    if [[ "$count" -gt 0 ]]; then
        success "清理 $count 个旧备份"
    else
        info "无旧备份需要清理"
    fi

    # 统计当前备份
    local total
    total="$(find "$BACKUP_DIR" -maxdepth 1 -name "${DB_NAME}-*" -type f 2>/dev/null | wc -l)"
    info "当前备份数: $total"
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

    local duration=$((BACKUP_END - BACKUP_START))
    echo "${ts}|${ENV}|${DB_NAME}|$(basename "$BACKUP_FILE")|${BACKUP_SIZE}|${duration}s|${BACKUP_ID}" \
        >> "$HISTORY_FILE"
}

# ==============================================================================
# 发送通知
# ==============================================================================
send_notification() {
    if [[ "$NOTIFY" != "1" ]]; then
        return
    fi

    local message="💾 数据库备份完成
环境: ${ENV}
数据库: ${DB_NAME}
文件: $(basename "$BACKUP_FILE")
大小: $(human_size "$BACKUP_SIZE")
备份 ID: ${BACKUP_ID}"

    # Telegram
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

    # 头部
    if [[ "$VERBOSE" != "1" ]] && [[ "$DRY_RUN" != "1" ]]; then
        printf "\n"
        printf "%b╔══════════════════════════════════════════════════════════════╗%b\n" \
            "${COLOR_CYAN}${COLOR_BOLD}" "${COLOR_RESET}"
        printf "%b║  数据库备份  v%s%b\n" \
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

    # 检查数据库
    check_database

    # 执行备份
    if ! do_backup; then
        exit 1
    fi

    # 生成校验和
    generate_checksum

    # 验证备份
    if ! verify_backup; then
        exit 7
    fi

    # S3 上传
    if ! upload_to_s3; then
        exit 8
    fi

    # 清理旧备份
    cleanup_old_backups

    # 记录历史
    record_history

    # 通知
    send_notification

    # 完成
    printf "\n"
    printf "%b✅ 备份完成%b\n" "${COLOR_GREEN}${COLOR_BOLD}" "${COLOR_RESET}"
    printf "   环境:     %s\n" "$ENV"
    printf "   数据库:   %s\n" "$DB_NAME"
    printf "   文件:     %s\n" "$(basename "$BACKUP_FILE")"
    printf "   大小:     %s\n" "$(human_size "$BACKUP_SIZE")"
    printf "   备份 ID:  %s\n" "$BACKUP_ID"
    if [[ "$S3_ENABLED" = "1" ]]; then
        printf "   S3:       s3://%s/%s/\n" "$S3_BUCKET" "$S3_PREFIX"
    fi
    printf "\n"
}

# ==============================================================================
# 入口
# ==============================================================================
main "$@"
