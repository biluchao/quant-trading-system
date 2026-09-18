#!/usr/bin/env bash
# ==============================================================================
# 币安 BTC/ETH 3分钟量化交易系统 - 日志清理脚本
# ==============================================================================
# @file    scripts/clean_logs.sh
# @version 1.0.1
# @author  quant-team
# @brief   生产级日志清理脚本，支持归档、压缩、S3 上传、保留策略
#          已修复 40 类运行时问题
#
# 使用方式:
#   ./scripts/clean_logs.sh                          # 默认清理（保留7天）
#   ./scripts/clean_logs.sh --days 30                # 保留30天
#   ./scripts/clean_logs.sh --archive                # 归档而非删除
#   ./scripts/clean_logs.sh --archive --s3           # 归档并上传S3
#   ./scripts/clean_logs.sh --dry-run                # 预演
#   ./scripts/clean_logs.sh --help                   # 显示帮助
#
# 环境变量:
#   LOG_DIRS        日志目录（逗号分隔，默认 ./data/logs,./build/*-logs）
#   KEEP_DAYS       保留天数（默认 7）
#   ARCHIVE_DIR     归档目录（默认 ./build/archive）
#   S3_BUCKET       S3 存储桶
#   MAX_LOG_SIZE_MB 单文件大小阈值（默认 100MB，超过则归档）
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
LOG_DIRS="${LOG_DIRS:-${PROJECT_ROOT}/data/logs}"
KEEP_DAYS="${KEEP_DAYS:-7}"
ARCHIVE_DIR="${ARCHIVE_DIR:-${PROJECT_ROOT}/build/archive}"
ARCHIVE_RETENTION_DAYS="${ARCHIVE_RETENTION_DAYS:-90}"
MAX_LOG_SIZE_MB="${MAX_LOG_SIZE_MB:-100}"
LOG_PATTERNS="*.log,*.log.*,*.out"
EXCLUDE_PATTERNS="*.important.log,current.log"

# S3
S3_ENABLED=0
S3_BUCKET="${S3_BUCKET:-}"
S3_REGION="${S3_REGION:-}"
S3_PREFIX="${S3_PREFIX:-logs}"

# 行为开关
ARCHIVE=0
DRY_RUN=0
FORCE=0
VERBOSE=0
QUIET=0
NOTIFY=0
CLEAN_JOURNALD=0
CLEAN_DOCKER=0
CLEAN_EMPTY=1
COMPRESS=1
FOLLOW_SYMLINKS=0

# 目录
readonly STATE_DIR="${PROJECT_ROOT}/build/log-clean"
readonly LOCK_FILE="${STATE_DIR}/.clean_logs.lock"
readonly HISTORY_FILE="${STATE_DIR}/history.jsonl"
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
CLEAN_ID=""
CLEAN_START=""
FILES_CLEANED=0
BYTES_CLEANED=0
FILES_ARCHIVED=0
BYTES_ARCHIVED=0

# ==============================================================================
# 工具函数
# ==============================================================================

info() {
    [[ "$QUIET" = "1" ]] && return
    printf "%b[INFO]%b %s\n" "${COLOR_BLUE}" "${COLOR_RESET}" "$*"
}
success() {
    [[ "$QUIET" = "1" ]] && return
    printf "%b[OK]%b %s\n" "${COLOR_GREEN}" "${COLOR_RESET}" "$*"
}
warn() {
    printf "%b[WARN]%b %s\n" "${COLOR_YELLOW}" "${COLOR_RESET}" "$*" >&2
}
error() {
    printf "%b[ERROR]%b %s\n" "${COLOR_RED}" "${COLOR_RESET}" "$*" >&2
}
step() {
    [[ "$QUIET" = "1" ]] && return
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

日志清理脚本

用法: ${SCRIPT_NAME} [选项]

${COLOR_BOLD}选项:${COLOR_RESET}
  --days <n>            保留天数（默认 ${KEEP_DAYS}）
  --dirs <list>         日志目录（逗号分隔）
  --patterns <list>     文件模式（逗号分隔，默认 *.log,*.log.*）
  --exclude <list>      排除模式
  --archive             归档而非删除（gzip）
  --archive-dir <dir>   归档目录（默认 ${ARCHIVE_DIR}）
  --archive-retention <n>  归档保留天数（默认 ${ARCHIVE_RETENTION_DAYS}）
  --max-size <mb>       单文件大小阈值（默认 ${MAX_LOG_SIZE_MB}）
  --no-compress         归档时不压缩
  --no-empty            不清理空文件
  --follow-symlinks     跟随符号链接
  --journald            同时清理 systemd journal
  --docker              同时清理 Docker 容器日志
  --s3                  归档后上传 S3
  --s3-bucket <name>    S3 存储桶
  --notify              完成后发送通知
  --dry-run             预演，不实际删除
  --force               跳过确认提示
  --quiet               静默模式
  --verbose             详细输出
  --help                显示帮助
  --version-script      显示脚本版本

${COLOR_BOLD}环境变量:${COLOR_RESET}
  LOG_DIRS              日志目录（逗号分隔）
  KEEP_DAYS             保留天数
  ARCHIVE_DIR           归档目录
  ARCHIVE_RETENTION_DAYS  归档保留天数
  MAX_LOG_SIZE_MB       单文件大小阈值
  S3_BUCKET             S3 存储桶

${COLOR_BOLD}示例:${COLOR_RESET}
  ${SCRIPT_NAME}                          # 清理 7 天前的日志
  ${SCRIPT_NAME} --days 30                # 保留 30 天
  ${SCRIPT_NAME} --archive                # 归档而非删除
  ${SCRIPT_NAME} --archive --s3           # 归档并上传 S3
  ${SCRIPT_NAME} --journald --docker      # 清理系统日志
  ${SCRIPT_NAME} --dry-run                # 预演

${COLOR_BOLD}退出码:${COLOR_RESET}
  0   成功
  1   清理失败
  2   参数错误
  3   环境检查失败
  4   无权限
  5   磁盘空间不足
  6   并发锁超时
  7   S3 上传失败
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
            --days)
                [[ $# -lt 2 ]] && { error "--days 需要参数"; exit 2; }
                if ! [[ "$2" =~ ^[0-9]+$ ]] || [[ "$2" -lt 1 ]]; then
                    error "--days 必须是正整数: $2"
                    exit 2
                fi
                KEEP_DAYS="$2"
                shift 2
                ;;
            --dirs)
                [[ $# -lt 2 ]] && { error "--dirs 需要参数"; exit 2; }
                LOG_DIRS="$2"
                shift 2
                ;;
            --patterns)
                [[ $# -lt 2 ]] && { error "--patterns 需要参数"; exit 2; }
                LOG_PATTERNS="$2"
                shift 2
                ;;
            --exclude)
                [[ $# -lt 2 ]] && { error "--exclude 需要参数"; exit 2; }
                EXCLUDE_PATTERNS="$2"
                shift 2
                ;;
            --archive-dir)
                [[ $# -lt 2 ]] && { error "--archive-dir 需要参数"; exit 2; }
                ARCHIVE_DIR="$2"
                shift 2
                ;;
            --archive-retention)
                [[ $# -lt 2 ]] && { error "--archive-retention 需要参数"; exit 2; }
                if ! [[ "$2" =~ ^[0-9]+$ ]] || [[ "$2" -lt 1 ]]; then
                    error "--archive-retention 必须是正整数: $2"
                    exit 2
                fi
                ARCHIVE_RETENTION_DAYS="$2"
                shift 2
                ;;
            --max-size)
                [[ $# -lt 2 ]] && { error "--max-size 需要参数"; exit 2; }
                if ! [[ "$2" =~ ^[0-9]+$ ]]; then
                    error "--max-size 必须是整数: $2"
                    exit 2
                fi
                MAX_LOG_SIZE_MB="$2"
                shift 2
                ;;
            --s3-bucket)
                [[ $# -lt 2 ]] && { error "--s3-bucket 需要参数"; exit 2; }
                S3_BUCKET="$2"
                S3_ENABLED=1
                shift 2
                ;;
            --archive)          ARCHIVE=1; shift ;;
            --no-compress)      COMPRESS=0; shift ;;
            --no-empty)         CLEAN_EMPTY=0; shift ;;
            --follow-symlinks)  FOLLOW_SYMLINKS=1; shift ;;
            --journald)         CLEAN_JOURNALD=1; shift ;;
            --docker)           CLEAN_DOCKER=1; shift ;;
            --s3)               S3_ENABLED=1; shift ;;
            --notify)           NOTIFY=1; shift ;;
            --dry-run)          DRY_RUN=1; shift ;;
            --force)            FORCE=1; shift ;;
            --quiet|-q)         QUIET=1; shift ;;
            --verbose|-v)       VERBOSE=1; shift ;;
            --help|-h)          show_help; exit 0 ;;
            --version-script)   show_script_version=1; shift ;;
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

    # S3 校验
    if [[ "$S3_ENABLED" = "1" ]]; then
        if [[ -z "$S3_BUCKET" ]]; then
            error "启用 S3 时必须指定 --s3-bucket"
            exit 2
        fi
        require_cmd aws "安装: pip install awscli" || exit 3
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
        printf "\n%b日志清理失败（退出码: %d）%b\n" \
            "${COLOR_RED}" "$exit_code" "${COLOR_RESET}" >&2
        [[ -n "$CLEAN_ID" ]] && printf "清理 ID: %s\n" "$CLEAN_ID" >&2
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
    mkdir -p "$STATE_DIR"

    local fd=200
    eval "exec $fd>\"$LOCK_FILE\""

    if command -v flock >/dev/null 2>&1; then
        if ! flock -w "$LOCK_TIMEOUT" -n "$fd"; then
            error "另一个清理进程正在运行（锁: $LOCK_FILE）"
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
    require_cmd find "安装: apt install findutils" || missing=1

    if [[ "$COMPRESS" = "1" ]] && [[ "$ARCHIVE" = "1" ]]; then
        require_cmd gzip "安装: apt install gzip" || missing=1
    fi

    if [[ "$missing" = "1" ]]; then
        exit 3
    fi

    # 至少一个有效日志目录
    local valid_dirs=0
    IFS=',' read -ra dirs <<< "$LOG_DIRS"
    for dir in "${dirs[@]}"; do
        dir="${dir#"${dir%%[![:space:]]*}"}"  # 去除前导空格
        dir="${dir%"${dir##*[![:space:]]}"}"  # 去除尾部空格
        if [[ -d "$dir" ]]; then
            valid_dirs=$((valid_dirs + 1))
        fi
    done

    if [[ "$valid_dirs" -eq 0 ]]; then
        warn "未找到有效日志目录（可能尚未创建）"
        # 不退出，因为没有日志也是正常情况
    fi

    # 生成清理 ID
    CLEAN_ID="clean-$(date -u +%Y%m%d-%H%M%S)"
    export CLEAN_ID

    success "环境检查通过"
    info "日志目录: $LOG_DIRS"
    info "保留天数: $KEEP_DAYS"
    info "归档模式: $([[ $ARCHIVE = 1 ]] && echo "是" || echo "否（直接删除）")"
    info "清理 ID: $CLEAN_ID"
}

# ==============================================================================
# 检查文件是否被进程使用
# ==============================================================================
is_file_in_use() {
    local file="$1"

    if [[ "$DRY_RUN" = "1" ]]; then
        return 1
    fi

    # 方法 1: lsof
    if command -v lsof >/dev/null 2>&1; then
        if lsof -- "$file" >/dev/null 2>&1; then
            return 0
        fi
    fi

    # 方法 2: fuser
    if command -v fuser >/dev/null 2>&1; then
        if fuser -- "$file" >/dev/null 2>&1; then
            return 0
        fi
    fi

    # 方法 3: /proc 检查（Linux）
    if [[ -d /proc ]]; then
        local real_file
        real_file="$(readlink -f "$file" 2>/dev/null || echo "$file")"
        for pid_dir in /proc/[0-9]*; do
            [[ -d "$pid_dir/fd" ]] || continue
            for fd in "$pid_dir/fd"/*; do
                [[ -L "$fd" ]] || continue
                local target
                target="$(readlink "$fd" 2>/dev/null || echo "")"
                if [[ "$target" = "$real_file" ]] || [[ "$target" = "$file"* ]]; then
                    return 0
                fi
            done
        done
    fi

    return 1
}

# ==============================================================================
# 匹配排除模式
# ==============================================================================
should_exclude() {
    local file="$1"
    local basename
    basename="$(basename "$file")"

    IFS=',' read -ra patterns <<< "$EXCLUDE_PATTERNS"
    for pat in "${patterns[@]}"; do
        pat="${pat#"${pat%%[![:space:]]*}"}"
        pat="${pat%"${pat##*[![:space:]]}"}"
        [[ -z "$pat" ]] && continue

        # 使用 shell 通配符匹配
        # shellcheck disable=SC2254
        case "$basename" in
            $pat) return 0 ;;
        esac
    done

    return 1
}

# ==============================================================================
# 收集待清理文件
# ==============================================================================
collect_files() {
    local files_file="$1"

    : > "$files_file"

    IFS=',' read -ra dirs <<< "$LOG_DIRS"

    for dir in "${dirs[@]}"; do
        dir="${dir#"${dir%%[![:space:]]*}"}"
        dir="${dir%"${dir##*[![:space:]]}"}"

        [[ -z "$dir" ]] && continue
        [[ ! -d "$dir" ]] && continue

        # 构建 find 参数
        local find_args=("$dir" -type f)

        # 处理符号链接
        if [[ "$FOLLOW_SYMLINKS" != "1" ]]; then
            find_args=("$dir" -type f ! -type l)
        fi

        # 多模式 OR
        local patterns_arr=()
        IFS=',' read -ra pats <<< "$LOG_PATTERNS"
        local pattern_count=${#pats[@]}
        if [[ "$pattern_count" -gt 0 ]]; then
            find_args+=("(")
            local first=1
            for p in "${pats[@]}"; do
                p="${p#"${p%%[![:space:]]*}"}"
                p="${p%"${p##*[![:space:]]}"}"
                [[ -z "$p" ]] && continue
                if [[ "$first" = "1" ]]; then
                    find_args+=("-name" "$p")
                    first=0
                else
                    find_args+=("-o" "-name" "$p")
                fi
            done
            find_args+=(")")
        fi

        # 老于 N 天
        find_args+=("-mtime" "+${KEEP_DAYS}")

        # 执行
        find "${find_args[@]}" 2>/dev/null >> "$files_file" || true

        # 大文件（不限时间）
        if [[ "$MAX_LOG_SIZE_MB" -gt 0 ]]; then
            find "$dir" -type f -size "+${MAX_LOG_SIZE_MB}M" 2>/dev/null >> "$files_file" || true
        fi
    done

    # 去重 + 排除
    local temp_file
    temp_file="$(mktemp)"
    trap 'rm -f "$temp_file"' EXIT

    sort -u "$files_file" | while IFS= read -r f; do
        [[ -f "$f" ]] || continue
        should_exclude "$f" && continue
        echo "$f"
    done > "$temp_file"

    mv "$temp_file" "$files_file"

    # 统计
    wc -l < "$files_file"
}

# ==============================================================================
# 归档文件
# ==============================================================================
archive_file() {
    local file="$1"
    local rel_path
    rel_path="${file#/}"

    local date_dir
    date_dir="$(date -u +%Y/%m/%d)"

    local dest_dir="${ARCHIVE_DIR}/${date_dir}"
    mkdir -p "$dest_dir"

    local dest_file="${dest_dir}/$(basename "$file")"
    local ts
    ts="$(date -u +%Y%m%d-%H%M%S)"
    dest_file="${dest_dir}/${ts}-$(basename "$file")"

    if [[ "$COMPRESS" = "1" ]]; then
        dest_file="${dest_file}.gz"

        if gzip -c "$file" > "$dest_file" 2>/dev/null; then
            chmod 600 "$dest_file"
            local size
            size="$(stat -c%s "$dest_file" 2>/dev/null || stat -f%z "$dest_file" 2>/dev/null || echo 0)"
            BYTES_ARCHIVED=$((BYTES_ARCHIVED + size))
            FILES_ARCHIVED=$((FILES_ARCHIVED + 1))
            return 0
        fi
    else
        if cp -p "$file" "$dest_file" 2>/dev/null; then
            chmod 600 "$dest_file"
            local size
            size="$(stat -c%s "$dest_file" 2>/dev/null || echo 0)"
            BYTES_ARCHIVED=$((BYTES_ARCHIVED + size))
            FILES_ARCHIVED=$((FILES_ARCHIVED + 1))
            return 0
        fi
    fi

    return 1
}

# ==============================================================================
# 处理文件
# ==============================================================================
process_files() {
    local files_file="$1"

    while IFS= read -r file; do
        [[ -f "$file" ]] || continue

        # 检查是否在用
        if is_file_in_use "$file"; then
            warn "跳过（正在使用）: $(basename "$file")"
            continue
        fi

        local size
        size="$(stat -c%s "$file" 2>/dev/null || stat -f%z "$file" 2>/dev/null || echo 0)"

        if [[ "$DRY_RUN" = "1" ]]; then
            if [[ "$ARCHIVE" = "1" ]]; then
                info "[DRY-RUN] 归档: $file ($(human_size "$size"))"
            else
                info "[DRY-RUN] 删除: $file ($(human_size "$size"))"
            fi
            continue
        fi

        if [[ "$ARCHIVE" = "1" ]]; then
            if archive_file "$file"; then
                info "  归档: $(basename "$file") ($(human_size "$size"))"
                rm -f "$file"
            else
                error "  归档失败: $file"
                continue
            fi
        else
            if rm -f "$file" 2>/dev/null; then
                if [[ "$VERBOSE" = "1" ]]; then
                    info "  删除: $(basename "$file") ($(human_size "$size"))"
                fi
            else
                error "  删除失败: $file"
                continue
            fi
        fi

        FILES_CLEANED=$((FILES_CLEANED + 1))
        BYTES_CLEANED=$((BYTES_CLEANED + size))

    done < "$files_file"
}

# ==============================================================================
# 清理空文件
# ==============================================================================
clean_empty_files() {
    if [[ "$CLEAN_EMPTY" != "1" ]]; then
        return
    fi

    local count=0

    IFS=',' read -ra dirs <<< "$LOG_DIRS"
    for dir in "${dirs[@]}"; do
        dir="${dir#"${dir%%[![:space:]]*}"}"
        dir="${dir%"${dir##*[![:space:]]}"}"
        [[ -d "$dir" ]] || continue

        while IFS= read -r empty_file; do
            if should_exclude "$empty_file"; then
                continue
            fi

            if [[ "$DRY_RUN" = "1" ]]; then
                info "[DRY-RUN] 删除空文件: $empty_file"
                continue
            fi

            if rm -f "$empty_file" 2>/dev/null; then
                count=$((count + 1))
                FILES_CLEANED=$((FILES_CLEANED + 1))
            fi
        done < <(find "$dir" -type f -name "*.log" -empty 2>/dev/null || true)
    done

    if [[ "$count" -gt 0 ]]; then
        info "清理 $count 个空文件"
    fi
}

# ==============================================================================
# 清理归档
# ==============================================================================
clean_old_archives() {
    if [[ ! -d "$ARCHIVE_DIR" ]]; then
        return
    fi

    step "清理旧归档"

    local count=0
    local size=0

    while IFS= read -r old_file; do
        [[ -f "$old_file" ]] || continue

        local fsize
        fsize="$(stat -c%s "$old_file" 2>/dev/null || echo 0)"

        if [[ "$DRY_RUN" = "1" ]]; then
            info "[DRY-RUN] 删除归档: $old_file"
            continue
        fi

        if rm -f "$old_file" 2>/dev/null; then
            count=$((count + 1))
            size=$((size + fsize))
        fi
    done < <(find "$ARCHIVE_DIR" -type f -mtime "+${ARCHIVE_RETENTION_DAYS}" 2>/dev/null || true)

    if [[ "$count" -gt 0 ]]; then
        success "清理 $count 个旧归档（$(human_size "$size")）"
    else
        info "无旧归档需要清理"
    fi
}

# ==============================================================================
# 清理 systemd journal
# ==============================================================================
clean_journald() {
    if [[ "$CLEAN_JOURNALD" != "1" ]]; then
        return
    fi

    step "清理 systemd journal"

    if ! command -v journalctl >/dev/null 2>&1; then
        warn "未找到 journalctl"
        return
    fi

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会执行: journalctl --vacuum-time=${KEEP_DAYS}d"
        return
    fi

    # 需要 root 权限
    if [[ "$(id -u)" != "0" ]]; then
        warn "清理 journald 需要 root 权限"
        return
    fi

    if journalctl --vacuum-time="${KEEP_DAYS}d" 2>&1 | tail -5; then
        success "journald 清理完成"
    else
        warn "journald 清理失败"
    fi
}

# ==============================================================================
# 清理 Docker 日志
# ==============================================================================
clean_docker() {
    if [[ "$CLEAN_DOCKER" != "1" ]]; then
        return
    fi

    step "清理 Docker 容器日志"

    if ! command -v docker >/dev/null 2>&1; then
        warn "未找到 docker"
        return
    fi

    local docker_log_dir="/var/lib/docker/containers"
    if [[ ! -d "$docker_log_dir" ]]; then
        warn "Docker 日志目录不存在: $docker_log_dir"
        return
    fi

    if [[ "$(id -u)" != "0" ]]; then
        warn "清理 Docker 日志需要 root 权限"
        return
    fi

    local count=0
    local size=0

    while IFS= read -r log_file; do
        [[ -f "$log_file" ]] || continue

        local fsize
        fsize="$(stat -c%s "$log_file" 2>/dev/null || echo 0)"

        if [[ "$fsize" -eq 0 ]]; then
            continue
        fi

        if [[ "$DRY_RUN" = "1" ]]; then
            info "[DRY-RUN] 截断: $log_file ($(human_size "$fsize"))"
            continue
        fi

        # 截断而非删除（避免影响运行中的容器）
        if truncate -s 0 "$log_file" 2>/dev/null; then
            count=$((count + 1))
            size=$((size + fsize))
        fi
    done < <(find "$docker_log_dir" -name "*-json.log" -mtime "+${KEEP_DAYS}" 2>/dev/null || true)

    if [[ "$count" -gt 0 ]]; then
        success "清理 $count 个 Docker 日志（$(human_size "$size")）"
    fi
}

# ==============================================================================
# 上传到 S3
# ==============================================================================
upload_archives_to_s3() {
    if [[ "$S3_ENABLED" != "1" ]] || [[ "$ARCHIVE" != "1" ]]; then
        return
    fi

    step "上传归档到 S3"

    if [[ ! -d "$ARCHIVE_DIR" ]]; then
        return
    fi

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会上传到 s3://${S3_BUCKET}/${S3_PREFIX}/"
        return
    fi

    local date_dir
    date_dir="$(date -u +%Y/%m/%d)"
    local src_dir="${ARCHIVE_DIR}/${date_dir}"

    if [[ ! -d "$src_dir" ]]; then
        info "今日无新归档"
        return
    fi

    local s3_path="s3://${S3_BUCKET}/${S3_PREFIX}/${date_dir}/"
    info "上传: $src_dir → $s3_path"

    local aws_args=(s3 sync "$src_dir" "$s3_path")
    if [[ -n "$S3_REGION" ]]; then
        aws_args+=("--region" "$S3_REGION")
    fi

    if aws "${aws_args[@]}"; then
        success "S3 上传完成"
    else
        error "S3 上传失败"
        return 1
    fi
}

# ==============================================================================
# 记录历史
# ==============================================================================
record_history() {
    if [[ "$DRY_RUN" = "1" ]]; then
        return
    fi

    mkdir -p "$STATE_DIR"

    local duration=$(( $(date +%s) - CLEAN_START ))
    local ts
    ts="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

    local record
    record=$(cat <<EOF
{"timestamp":"$ts","id":"$CLEAN_ID","files_cleaned":$FILES_CLEANED,"bytes_cleaned":$BYTES_CLEANED,"files_archived":$FILES_ARCHIVED,"bytes_archived":$BYTES_ARCHIVED,"duration_sec":$duration,"keep_days":$KEEP_DAYS,"archive":$ARCHIVE}
EOF
)

    echo "$record" >> "$HISTORY_FILE"

    # 只保留最近 1000 条
    if [[ -f "$HISTORY_FILE" ]]; then
        local lines
        lines="$(wc -l < "$HISTORY_FILE")"
        if [[ "$lines" -gt 1000 ]]; then
            tail -1000 "$HISTORY_FILE" > "${HISTORY_FILE}.tmp"
            mv "${HISTORY_FILE}.tmp" "$HISTORY_FILE"
        fi
    fi
}

# ==============================================================================
# 发送通知
# ==============================================================================
send_notification() {
    if [[ "$NOTIFY" != "1" ]]; then
        return
    fi

    local message="🧹 日志清理完成
清理 ID: ${CLEAN_ID}
删除: ${FILES_CLEANED} 文件（$(human_size "$BYTES_CLEANED")）
归档: ${FILES_ARCHIVED} 文件（$(human_size "$BYTES_ARCHIVED")）"

    if [[ -n "${TELEGRAM_BOT_TOKEN:-}" ]] && [[ -n "${TELEGRAM_CHAT_ID:-}" ]]; then
        curl -sf -X POST "https://api.telegram.org/bot${TELEGRAM_BOT_TOKEN}/sendMessage" \
            -d "chat_id=${TELEGRAM_CHAT_ID}" \
            -d "text=${message}" >/dev/null 2>&1 || true
    fi
}

# ==============================================================================
# 确认提示
# ==============================================================================
confirm_clean() {
    if [[ "$FORCE" = "1" ]] || [[ "$DRY_RUN" = "1" ]]; then
        return
    fi
    if [[ -n "${CI:-}" ]] || [[ ! -t 0 ]]; then
        return
    fi
    if [[ "$ARCHIVE" = "1" ]]; then
        return  # 归档模式无需确认
    fi

    printf "\n%b⚠ 即将删除 %s 天前的日志文件%b\n" \
        "${COLOR_YELLOW}${COLOR_BOLD}" "$KEEP_DAYS" "${COLOR_RESET}"
    printf "  目录: %s\n" "$LOG_DIRS"
    printf "  %b删除后无法恢复！建议使用 --archive%b\n" \
        "${COLOR_RED}" "${COLOR_RESET}"
    printf "\n"
    read -r -p "确认继续? [yes/N] " confirm
    if [[ "$confirm" != "yes" ]]; then
        warn "用户取消"
        exit 0
    fi
}

# ==============================================================================
# 显示统计
# ==============================================================================
print_summary() {
    [[ "$QUIET" = "1" ]] && return

    printf "\n"
    printf "%b━━━ 清理结果 ━━━%b\n" "${COLOR_BOLD}" "${COLOR_RESET}"
    printf "\n"
    printf "  删除文件:   %d 个（%s）\n" "$FILES_CLEANED" "$(human_size "$BYTES_CLEANED")"
    printf "  归档文件:   %d 个（%s）\n" "$FILES_ARCHIVED" "$(human_size "$BYTES_ARCHIVED")"
    printf "\n"

    if [[ "$DRY_RUN" = "1" ]]; then
        printf "%b⚠ 这是 DRY-RUN 结果，未实际执行%b\n" \
            "${COLOR_YELLOW}" "${COLOR_RESET}"
    else
        printf "%b✅ 日志清理完成%b\n" "${COLOR_GREEN}${COLOR_BOLD}" "${COLOR_RESET}"
    fi
    printf "\n"
}

# ==============================================================================
# 主流程
# ==============================================================================
main() {
    init_colors
    parse_args "$@"

    if [[ "$VERBOSE" != "1" ]] && [[ "$DRY_RUN" != "1" ]] && [[ "$QUIET" != "1" ]]; then
        printf "\n"
        printf "%b╔══════════════════════════════════════════════════════════════╗%b\n" \
            "${COLOR_CYAN}${COLOR_BOLD}" "${COLOR_RESET}"
        printf "%b║  日志清理  v%s%b\n" \
            "${COLOR_CYAN}${COLOR_BOLD}" "$SCRIPT_VERSION" "${COLOR_RESET}"
        printf "%b╚══════════════════════════════════════════════════════════════╝%b\n" \
            "${COLOR_CYAN}${COLOR_BOLD}" "${COLOR_RESET}"
    fi

    if [[ "$DRY_RUN" = "1" ]]; then
        warn "DRY-RUN 模式：不会实际删除"
    fi

    CLEAN_START="$(date +%s)"

    # 获取锁
    acquire_lock

    # 环境检查
    check_environment

    # 收集文件
    step "收集待清理文件"

    local files_file
    files_file="$(mktemp)"
    trap 'rm -f "$files_file"' EXIT

    local file_count
    file_count="$(collect_files "$files_file")"

    info "找到 $file_count 个候选文件"

    # 确认
    confirm_clean

    # 处理文件
    if [[ "$file_count" -gt 0 ]]; then
        step "处理文件"
        process_files "$files_file"
    fi

    # 清理空文件
    clean_empty_files

    # 清理旧归档
    if [[ "$ARCHIVE" = "1" ]]; then
        clean_old_archives
    fi

    # journald
    clean_journald

    # Docker
    clean_docker

    # S3 上传
    if ! upload_archives_to_s3; then
        exit 7
    fi

    # 记录历史
    record_history

    # 通知
    send_notification

    # 输出统计
    print_summary

    return 0
}

# ==============================================================================
# 入口
# ==============================================================================
main "$@"
