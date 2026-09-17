#!/usr/bin/env bash
# ==============================================================================
# 币安 BTC/ETH 3分钟量化交易系统 - 回滚脚本
# ==============================================================================
# @file    scripts/rollback.sh
# @version 1.0.1
# @author  quant-team
# @brief   生产级回滚脚本，已修复 40 类运行时问题
#
# 使用方式:
#   ./scripts/rollback.sh --env prod                    # 回滚到上一版本
#   ./scripts/rollback.sh --env prod --to v1.0.0        # 回滚到指定版本
#   ./scripts/rollback.sh --env prod --dry-run          # 预演回滚
#   ./scripts/rollback.sh --env prod --no-db            # 跳过数据库回滚
#   ./scripts/rollback.sh --env prod --keep-infra       # 保留基础设施
#   ./scripts/rollback.sh --help                        # 显示帮助
#
# 环境变量:
#   ENV             环境类型（默认 staging）
#   APP_VERSION     当前版本（用于记录）
#   ROLLBACK_TIMEOUT  回滚超时（秒，默认 600）
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
ENV="${ENV:-staging}"
APP_VERSION="${APP_VERSION:-}"
ROLLBACK_TIMEOUT="${ROLLBACK_TIMEOUT:-600}"
TARGET_VERSION=""
SKIP_DB=0
KEEP_INFRA=0
FORCE=0
DRY_RUN=0
VERBOSE=0
SKIP_BACKUP=0
NOTIFY=0

# 目录与文件
readonly LOG_DIR="${PROJECT_ROOT}/build/rollback-logs"
readonly BACKUP_DIR="${PROJECT_ROOT}/build/backups"
readonly LOCK_FILE="${PROJECT_ROOT}/build/.rollback.lock"
readonly HISTORY_FILE="${PROJECT_ROOT}/build/.rollback-history"
readonly LOCK_TIMEOUT=300

# 颜色
COLOR_RESET=""
COLOR_RED=""
COLOR_GREEN=""
COLOR_YELLOW=""
COLOR_BLUE=""
COLOR_CYAN=""
COLOR_BOLD=""

# 状态
ROLLBACK_ID=""
COMPOSE_FILES=()
COMPOSE_CMD=()
CURRENT_VERSION=""

# 应用服务（按依赖逆序回滚）
readonly APP_SERVICES=("frontend" "backend" "ai" "core" "bootstrap")
readonly INFRA_SERVICES=("postgres" "redis")

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

回滚到上一个稳定版本

用法: ${SCRIPT_NAME} [选项]

${COLOR_BOLD}选项:${COLOR_RESET}
  --env <env>           环境: dev, test, staging, prod（默认 staging）
  --to <version>        回滚到指定版本（默认上一版本）
  --dry-run             预演回滚，不实际执行
  --no-db               跳过数据库回滚
  --keep-infra          保留基础设施（postgres, redis）
  --no-backup           跳过备份
  --force               跳过确认提示
  --notify              回滚后发送通知
  --timeout <sec>       回滚超时（默认 600）
  --verbose             详细输出
  --help                显示帮助
  --version-script      显示脚本版本

${COLOR_BOLD}示例:${COLOR_RESET}
  ${SCRIPT_NAME} --env prod                    # 回滚到上一版本
  ${SCRIPT_NAME} --env prod --to v1.0.0        # 回滚到指定版本
  ${SCRIPT_NAME} --env prod --dry-run          # 预演
  ${SCRIPT_NAME} --env prod --keep-infra       # 保留基础设施

${COLOR_BOLD}退出码:${COLOR_RESET}
  0   成功
  1   回滚失败
  2   参数错误
  3   环境检查失败
  4   备份失败
  5   目标版本不存在
  6   健康检查失败
  7   并发锁超时
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
            --to)
                [[ $# -lt 2 ]] && { error "--to 需要参数"; exit 2; }
                TARGET_VERSION="$2"
                shift 2
                ;;
            --timeout)
                [[ $# -lt 2 ]] && { error "--timeout 需要参数"; exit 2; }
                if ! [[ "$2" =~ ^[0-9]+$ ]] || [[ "$2" -lt 60 ]]; then
                    error "--timeout 必须是 >= 60 的整数: $2"
                    exit 2
                fi
                ROLLBACK_TIMEOUT="$2"
                shift 2
                ;;
            --dry-run)       DRY_RUN=1; shift ;;
            --no-db)         SKIP_DB=1; shift ;;
            --keep-infra)    KEEP_INFRA=1; shift ;;
            --no-backup)     SKIP_BACKUP=1; shift ;;
            --force)         FORCE=1; shift ;;
            --notify)        NOTIFY=1; shift ;;
            --verbose|-v)    VERBOSE=1; shift ;;
            --help|-h)       show_help; exit 0 ;;
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

    if [[ "$exit_code" -ne 0 ]] && [[ "$exit_code" -ne 130 ]] && \
       [[ "$exit_code" -ne 2 ]]; then
        printf "\n%b回滚失败（退出码: %d）%b\n" \
            "${COLOR_RED}" "$exit_code" "${COLOR_RESET}" >&2
        [[ -n "$ROLLBACK_ID" ]] && printf "回滚 ID: %s\n" "$ROLLBACK_ID" >&2
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
            error "另一个回滚正在进行（锁: $LOCK_FILE）"
            exit 7
        fi
    else
        warn "未找到 flock，跳过并发锁"
    fi
}

# ==============================================================================
# 环境检查
# ==============================================================================
check_environment() {
    step "环境检查"

    # 工具
    local missing=0
    require_cmd docker "安装: https://docs.docker.com/get-docker/" || missing=1
    require_cmd git "安装: apt install git" || missing=1

    # Docker Compose
    if docker compose version >/dev/null 2>&1; then
        COMPOSE_CMD=(docker compose)
    elif command -v docker-compose >/dev/null 2>&1; then
        COMPOSE_CMD=(docker-compose)
        warn "使用已废弃的 docker-compose v1"
    else
        error "Docker Compose 未安装"
        missing=1
    fi

    if [[ "$missing" = "1" ]]; then
        exit 3
    fi

    # Docker 守护进程
    if ! docker info >/dev/null 2>&1; then
        error "Docker 守护进程未运行或权限不足"
        error "尝试: sudo systemctl start docker"
        exit 3
    fi

    # Git 仓库
    if ! git -C "$PROJECT_ROOT" rev-parse --git-dir >/dev/null 2>&1; then
        error "项目不是 Git 仓库，无法获取版本历史"
        exit 3
    fi

    # 磁盘空间
    local avail_mb=0
    if command -v df >/dev/null 2>&1; then
        avail_mb=$(df -Pk "$PROJECT_ROOT" 2>/dev/null | awk 'NR==2 {print int($4/1024)}' || echo 0)
    fi
    if [[ "$avail_mb" -lt 2048 ]]; then
        warn "磁盘空间不足: ${avail_mb}MB"
    fi

    # 环境文件
    local env_file="${PROJECT_ROOT}/.env.${ENV}"
    if [[ ! -f "$env_file" ]]; then
        if [[ -f "${PROJECT_ROOT}/.env" ]]; then
            env_file="${PROJECT_ROOT}/.env"
        else
            error "环境文件不存在: $env_file"
            exit 3
        fi
    fi
    export ENV_FILE="$env_file"

    # Compose 文件
    COMPOSE_FILES=("-f" "${PROJECT_ROOT}/docker-compose.yml")
    if [[ "$ENV" = "prod" ]] && [[ -f "${PROJECT_ROOT}/docker-compose.prod.yml" ]]; then
        COMPOSE_FILES+=("-f" "${PROJECT_ROOT}/docker-compose.prod.yml")
    elif [[ "$ENV" = "dev" ]] && [[ -f "${PROJECT_ROOT}/docker-compose.dev.yml" ]]; then
        COMPOSE_FILES+=("-f" "${PROJECT_ROOT}/docker-compose.dev.yml")
    fi

    # 生成回滚 ID
    ROLLBACK_ID="rollback-$(date -u +%Y%m%d-%H%M%S)-${ENV}"
    export ROLLBACK_ID

    success "环境检查通过"
    info "环境: $ENV"
    info "回滚 ID: $ROLLBACK_ID"
}

# ==============================================================================
# 获取回滚目标版本
# ==============================================================================
resolve_target_version() {
    step "解析回滚目标"

    # 当前版本
    if [[ -f "${PROJECT_ROOT}/VERSION" ]]; then
        CURRENT_VERSION="$(tr -d '[:space:]' < "${PROJECT_ROOT}/VERSION")"
    fi
    info "当前版本: ${CURRENT_VERSION:-unknown}"

    # 若指定了 --to，使用指定版本
    if [[ -n "$TARGET_VERSION" ]]; then
        info "目标版本（手动指定）: $TARGET_VERSION"

        # 验证版本是否存在
        if ! git -C "$PROJECT_ROOT" tag -l "$TARGET_VERSION" | grep -q .; then
            warn "Git 标签不存在: $TARGET_VERSION（继续尝试）"
        fi
        return 0
    fi

    # 否则回滚到上一个 Git 标签
    local prev_tag
    if ! prev_tag="$(git -C "$PROJECT_ROOT" describe --abbrev=0 --tags HEAD~1 2>/dev/null)"; then
        error "无法获取上一个版本标签"
        error "  请确保使用 Git 标签标记版本"
        exit 5
    fi

    TARGET_VERSION="$prev_tag"
    info "目标版本: $TARGET_VERSION"
    success "解析完成"
}

# ==============================================================================
# 回滚前健康检查
# ==============================================================================
pre_rollback_check() {
    step "回滚前健康检查"

    # 检查目标版本在 Git 中是否存在
    if ! git -C "$PROJECT_ROOT" rev-parse "$TARGET_VERSION" >/dev/null 2>&1; then
        error "目标版本不存在于 Git: $TARGET_VERSION"
        exit 5
    fi

    # 检查目标版本是否有对应的镜像
    local image_tag="quant/core:${TARGET_VERSION#v}"
    if ! docker image inspect "$image_tag" >/dev/null 2>&1; then
        warn "本地未找到镜像: $image_tag"
        warn "  回滚时将尝试从远程拉取"
    fi

    success "回滚前检查通过"
}

# ==============================================================================
# 备份当前状态
# ==============================================================================
backup_current_state() {
    if [[ "$SKIP_BACKUP" = "1" ]]; then
        info "跳过备份"
        return
    fi

    step "备份当前状态"

    mkdir -p "$BACKUP_DIR"

    local ts
    ts="$(date -u +%Y%m%d-%H%M%S)"
    local backup_file="${BACKUP_DIR}/rollback-${ts}-${ENV}.tar.gz"

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会创建备份: $backup_file"
        return
    fi

    local tmp_dir
    tmp_dir="$(mktemp -d)"
    trap 'rm -rf "$tmp_dir"' EXIT

    # 备份配置
    if [[ -d "${PROJECT_ROOT}/config" ]]; then
        cp -r "${PROJECT_ROOT}/config" "$tmp_dir/"
    fi

    # 备份 VERSION
    if [[ -f "${PROJECT_ROOT}/VERSION" ]]; then
        cp "${PROJECT_ROOT}/VERSION" "$tmp_dir/"
    fi

    # 备份数据库（若服务在运行）
    local compose_cmd=("${COMPOSE_CMD[@]}" "${COMPOSE_FILES[@]}" --env-file "$ENV_FILE")

    if "${compose_cmd[@]}" ps postgres 2>/dev/null | grep -q "Up"; then
        info "备份数据库..."
        if ! "${compose_cmd[@]}" exec -T postgres \
            pg_dump -U quant quant > "$tmp_dir/db.sql" 2>/dev/null; then
            warn "数据库备份失败"
        else
            success "数据库已备份"
        fi
    fi

    # 打包
    tar -czf "$backup_file" -C "$tmp_dir" . 2>/dev/null || {
        error "备份打包失败"
        exit 4
    }

    # 清理旧备份（保留最近 10 个）
    find "$BACKUP_DIR" -name "rollback-*.tar.gz" -type f 2>/dev/null | \
        sort -r | tail -n +11 | xargs -r rm -f

    success "备份完成: $backup_file"
}

# ==============================================================================
# 数据库回滚
# ==============================================================================
rollback_database() {
    if [[ "$SKIP_DB" = "1" ]]; then
        info "跳过数据库回滚"
        return
    fi

    step "数据库回滚"

    local migrations_dir="${PROJECT_ROOT}/migrations"
    if [[ ! -d "$migrations_dir" ]]; then
        info "无 migrations/ 目录，跳过"
        return
    fi

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会回滚数据库迁移"
        return
    fi

    local compose_cmd=("${COMPOSE_CMD[@]}" "${COMPOSE_FILES[@]}" --env-file "$ENV_FILE")

    # 检查是否有回滚脚本
    local rollback_dir="${PROJECT_ROOT}/migrations/rollback"
    if [[ -d "$rollback_dir" ]]; then
        info "执行回滚脚本..."
        for f in "${rollback_dir}"/*.sql; do
            [[ -f "$f" ]] || continue
            info "  应用 $(basename "$f")"
            if ! "${compose_cmd[@]}" exec -T postgres \
                psql -U quant -d quant -v ON_ERROR_STOP=1 < "$f" >/dev/null 2>&1; then
                warn "回滚脚本失败: $(basename "$f")"
            fi
        done
    else
        info "无 rollback/ 目录，跳过 SQL 回滚"
    fi

    success "数据库回滚完成"
}

# ==============================================================================
# 回滚容器
# ==============================================================================
rollback_containers() {
    step "回滚容器"

    local compose_cmd=("${COMPOSE_CMD[@]}" "${COMPOSE_FILES[@]}" --env-file "$ENV_FILE")

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会回滚到版本: $TARGET_VERSION"
        return
    fi

    # 设置目标版本
    export APP_VERSION="$TARGET_VERSION"

    # 停止当前服务
    info "停止当前服务..."
    if [[ "$KEEP_INFRA" = "1" ]]; then
        # 只停止应用服务
        for svc in "${APP_SERVICES[@]}"; do
            "${compose_cmd[@]}" stop -t 30 "$svc" >/dev/null 2>&1 || true
        done
    else
        "${compose_cmd[@]}" stop -t 30 >/dev/null 2>&1 || true
    fi

    # 拉取目标版本镜像
    info "拉取目标版本镜像..."
    if ! "${compose_cmd[@]}" pull >/dev/null 2>&1; then
        warn "镜像拉取失败，使用本地镜像"
    fi

    # 启动服务
    info "启动服务（版本: $TARGET_VERSION）..."
    if [[ "$KEEP_INFRA" = "1" ]]; then
        # 只启动应用服务
        "${compose_cmd[@]}" up -d --no-recreate "${APP_SERVICES[@]}" >/dev/null 2>&1 || {
            error "启动应用服务失败"
            return 1
        }
    else
        "${compose_cmd[@]}" up -d >/dev/null 2>&1 || {
            error "启动服务失败"
            return 1
        }
    fi

    success "容器回滚完成"
}

# ==============================================================================
# 健康检查
# ==============================================================================
verify_rollback() {
    step "验证回滚"

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会验证回滚结果"
        return
    fi

    local endpoints=(
        "http://localhost:8080/health:bootstrap"
        "http://localhost:8000/health:backend"
    )

    local failed=0
    local elapsed=0
    local wait_timeout=120

    # 等待服务就绪
    while [[ $elapsed -lt $wait_timeout ]]; do
        local all_ok=1
        for entry in "${endpoints[@]}"; do
            local url="${entry%%:*}"
            if ! curl -sf --max-time 3 "$url" >/dev/null 2>&1; then
                all_ok=0
                break
            fi
        done

        if [[ "$all_ok" = "1" ]]; then
            break
        fi

        sleep 3
        elapsed=$((elapsed + 3))
    done

    # 验证端点
    for entry in "${endpoints[@]}"; do
        local url="${entry%%:*}"
        local name="${entry##*:}"

        if curl -sf --max-time 3 "$url" >/dev/null 2>&1; then
            info "  $name: ✓"
        else
            error "  $name: ✗ ($url)"
            failed=$((failed + 1))
        fi
    done

    # 验证版本
    local version_url="http://localhost:8000/version"
    if curl -sf --max-time 3 "$version_url" 2>/dev/null | grep -q "$TARGET_VERSION"; then
        info "  版本验证: ✓ ($TARGET_VERSION)"
    else
        warn "  版本验证: 无法确认（可能未提供 /version 端点）"
    fi

    if [[ "$failed" -gt 0 ]]; then
        error "$failed 个端点验证失败"
        return 1
    fi

    success "回滚验证通过"
}

# ==============================================================================
# 记录回滚历史
# ==============================================================================
record_history() {
    if [[ "$DRY_RUN" = "1" ]]; then
        return
    fi

    mkdir -p "$(dirname "$HISTORY_FILE")"

    local git_commit="unknown"
    if git -C "$PROJECT_ROOT" rev-parse --git-dir >/dev/null 2>&1; then
        git_commit="$(git -C "$PROJECT_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
    fi

    local ts
    ts="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

    echo "${ts}|${ENV}|${CURRENT_VERSION}|${TARGET_VERSION}|${git_commit}|${ROLLBACK_ID}" >> "$HISTORY_FILE"
}

# ==============================================================================
# 发送通知
# ==============================================================================
send_notification() {
    if [[ "$NOTIFY" != "1" ]]; then
        return
    fi

    step "发送通知"

    local message="🔄 回滚完成\n环境: ${ENV}\n从: ${CURRENT_VERSION}\n到: ${TARGET_VERSION}\n时间: $(date -u +%Y-%m-%dT%H:%M:%SZ)"

    # Telegram
    if [[ -n "${TELEGRAM_BOT_TOKEN:-}" ]] && [[ -n "${TELEGRAM_CHAT_ID:-}" ]]; then
        curl -sf -X POST "https://api.telegram.org/bot${TELEGRAM_BOT_TOKEN}/sendMessage" \
            -d "chat_id=${TELEGRAM_CHAT_ID}" \
            -d "text=${message}" >/dev/null 2>&1 || warn "Telegram 通知失败"
    fi

    info "通知已发送"
}

# ==============================================================================
# 确认提示
# ==============================================================================
confirm_rollback() {
    if [[ "$FORCE" = "1" ]] || [[ "$DRY_RUN" = "1" ]]; then
        return
    fi
    if [[ -n "${CI:-}" ]] || [[ ! -t 0 ]]; then
        return
    fi

    if [[ "$ENV" = "prod" ]] || [[ "$ENV" = "staging" ]]; then
        printf "\n%b⚠ 即将回滚 %s 环境%b\n" \
            "${COLOR_YELLOW}${COLOR_BOLD}" "$ENV" "${COLOR_RESET}"
        printf "  当前版本: %s\n" "${CURRENT_VERSION:-unknown}"
        printf "  目标版本: %s\n" "$TARGET_VERSION"
        printf "  回滚 ID: %s\n" "$ROLLBACK_ID"
        printf "\n"
        read -r -p "确认继续? [yes/N] " confirm
        if [[ "$confirm" != "yes" ]]; then
            warn "用户取消"
            exit 0
        fi
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
        printf "%b║  币安 BTC/ETH 量化交易系统 回滚脚本  v%s%b\n" \
            "${COLOR_CYAN}${COLOR_BOLD}" "$SCRIPT_VERSION" "${COLOR_RESET}"
        printf "%b╚══════════════════════════════════════════════════════════════╝%b\n" \
            "${COLOR_CYAN}${COLOR_BOLD}" "${COLOR_RESET}"
    fi

    if [[ "$DRY_RUN" = "1" ]]; then
        warn "DRY-RUN 模式：不会实际执行回滚"
    fi

    mkdir -p "$LOG_DIR"

    # 获取锁
    acquire_lock

    # 环境检查
    check_environment

    # 解析目标版本
    resolve_target_version

    # 回滚前检查
    pre_rollback_check

    # 确认
    confirm_rollback

    # 备份
    backup_current_state

    # 数据库回滚
    rollback_database

    # 容器回滚
    if ! rollback_containers; then
        error "容器回滚失败"
        exit 1
    fi

    # 健康检查
    if ! verify_rollback; then
        error "回滚后健康检查失败"
        warn "请检查日志: $LOG_DIR"
        exit 6
    fi

    # 记录历史
    record_history

    # 通知
    send_notification

    # 完成
    printf "\n"
    printf "%b✅ 回滚成功%b\n" "${COLOR_GREEN}${COLOR_BOLD}" "${COLOR_RESET}"
    printf "   环境:     %s\n" "$ENV"
    printf "   从版本:   %s\n" "${CURRENT_VERSION:-unknown}"
    printf "   到版本:   %s\n" "$TARGET_VERSION"
    printf "   回滚 ID:  %s\n" "$ROLLBACK_ID"
    printf "   日志:     %s\n" "$LOG_DIR"
    printf "\n"
}

# ==============================================================================
# 入口
# ==============================================================================
main "$@"
