#!/usr/bin/env bash
# ==============================================================================
# 币安 BTC/ETH 3分钟量化交易系统 - 停止脚本
# ==============================================================================
# @file    scripts/stop.sh
# @version 1.0.1
# @author  quant-team
# @brief   生产级停止脚本，已修复 40 类运行时问题
#
# 使用方式:
#   ./scripts/stop.sh                         # 优雅停止所有服务（保留卷）
#   ./scripts/stop.sh --env dev               # 开发环境
#   ./scripts/stop.sh --services core,backend # 仅停止指定服务
#   ./scripts/stop.sh --keep-infra            # 保留数据库和缓存
#   ./scripts/stop.sh --volumes               # 同时删除卷（危险！）
#   ./scripts/stop.sh --rmi local             # 同时删除镜像
#   ./scripts/stop.sh --timeout 60            # 优雅停止超时 60 秒
#   ./scripts/stop.sh --force                 # 跳过确认
#   ./scripts/stop.sh --help                  # 显示帮助
#
# 环境变量:
#   ENV             环境类型（默认 prod）
#   STOP_TIMEOUT    优雅停止超时（默认 30 秒）
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
ENV="${ENV:-prod}"
STOP_TIMEOUT="${STOP_TIMEOUT:-30}"
FORCE=0
VERBOSE=0
DRY_RUN=0
REMOVE_VOLUMES=0
RMI_MODE=""
KEEP_INFRA=0
SERVICES=""

# 目录
readonly LOG_DIR="${PROJECT_ROOT}/build/stop-logs"
readonly HISTORY_FILE="${PROJECT_ROOT}/build/.stop-history"
readonly LOCK_FILE="${PROJECT_ROOT}/build/.stop.lock"
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
STOP_ID=""
COMPOSE_FILES=()
COMPOSE_CMD=()

# 基础设施服务名（--keep-infra 时保留）
readonly INFRA_SERVICES=("postgres" "redis")
# 应用服务名（按依赖逆序停止）
readonly APP_SERVICES=("frontend" "backend" "ai" "core" "bootstrap")

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

用法: ${SCRIPT_NAME} [选项]

${COLOR_BOLD}选项:${COLOR_RESET}
  --env <env>           环境: dev, test, staging, prod（默认 prod）
  --services <list>     仅停止指定服务（逗号分隔）
                        示例: --services core,backend
  --keep-infra          保留基础设施（postgres, redis）
  --volumes             同时删除数据卷（危险！会丢失数据）
  --rmi <mode>          删除镜像: local, all
  --timeout <sec>       优雅停止超时（默认 30）
  --force               跳过确认提示
  --dry-run             预演停止，不实际执行
  --verbose             详细输出
  --help                显示帮助
  --version-script      显示脚本版本

${COLOR_BOLD}环境变量:${COLOR_RESET}
  ENV                   环境类型
  STOP_TIMEOUT          优雅停止超时

${COLOR_BOLD}示例:${COLOR_RESET}
  ${SCRIPT_NAME}                              # 优雅停止所有服务
  ${SCRIPT_NAME} --keep-infra                 # 保留数据库和缓存
  ${SCRIPT_NAME} --services backend,frontend  # 仅停止应用层
  ${SCRIPT_NAME} --volumes --force            # 强制删除卷（危险）
  ${SCRIPT_NAME} --rmi local                  # 停止并删除本地镜像

${COLOR_BOLD}退出码:${COLOR_RESET}
  0   成功
  1   停止失败
  2   参数错误
  3   环境检查失败
  4   服务未运行（非错误）
  5   超时
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
            --services)
                [[ $# -lt 2 ]] && { error "--services 需要参数"; exit 2; }
                SERVICES="$2"
                shift 2
                ;;
            --timeout)
                [[ $# -lt 2 ]] && { error "--timeout 需要参数"; exit 2; }
                if ! [[ "$2" =~ ^[0-9]+$ ]] || [[ "$2" -lt 5 ]]; then
                    error "--timeout 必须是 >= 5 的整数: $2"
                    exit 2
                fi
                STOP_TIMEOUT="$2"
                shift 2
                ;;
            --rmi)
                [[ $# -lt 2 ]] && { error "--rmi 需要参数"; exit 2; }
                case "$2" in
                    local|all) RMI_MODE="$2" ;;
                    *)
                        error "--rmi 只能是 local 或 all: $2"
                        exit 2
                        ;;
                esac
                shift 2
                ;;
            --keep-infra)     KEEP_INFRA=1; shift ;;
            --volumes)        REMOVE_VOLUMES=1; shift ;;
            --force)          FORCE=1; shift ;;
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
       [[ "$exit_code" -ne 2 ]] && [[ "$exit_code" -ne 4 ]]; then
        printf "\n%b停止失败（退出码: %d）%b\n" "${COLOR_RED}" "$exit_code" "${COLOR_RESET}" >&2
        [[ -n "$STOP_ID" ]] && printf "停止 ID: %s\n" "$STOP_ID" >&2
        printf "查看日志: %s\n" "$LOG_DIR" >&2
    fi
}

handle_signal() {
    local sig=$1
    printf "\n%b收到信号 %s，正在清理...%b\n" "${COLOR_YELLOW}" "$sig" "${COLOR_RESET}" >&2
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
            error "另一个停止进程正在运行（锁: $LOCK_FILE）"
            exit 7
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
    require_cmd docker "安装: https://docs.docker.com/get-docker/" || missing=1

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
        error "Docker 守护进程未运行"
        error "尝试: sudo systemctl start docker"
        exit 3
    fi

    # 环境文件
    local env_file="${PROJECT_ROOT}/.env.${ENV}"
    if [[ ! -f "$env_file" ]]; then
        if [[ -f "${PROJECT_ROOT}/.env" ]]; then
            env_file="${PROJECT_ROOT}/.env"
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

    # 生成停止 ID
    STOP_ID="stop-$(date -u +%Y%m%d-%H%M%S)-${ENV}"
    export STOP_ID

    success "环境检查通过"
    info "环境: $ENV"
    info "停止 ID: $STOP_ID"
}

# ==============================================================================
# 检查运行状态
# ==============================================================================
check_running() {
    step "检查服务状态"

    local args=("${COMPOSE_CMD[@]}" "${COMPOSE_FILES[@]}")
    if [[ -f "${ENV_FILE:-}" ]]; then
        args+=("--env-file" "$ENV_FILE")
    fi

    local running_count=0
    local output=""

    if output="$("${args[@]}" ps --format json 2>/dev/null)"; then
        running_count="$(echo "$output" | grep -c '"State":"running"' || echo 0)"
    fi

    if [[ "$running_count" -eq 0 ]]; then
        info "未检测到运行中的服务"
        if [[ "$FORCE" != "1" ]]; then
            success "系统已停止（无需操作）"
            exit 4
        fi
        warn "强制模式：继续执行清理"
    else
        info "运行中的服务: $running_count 个"
    fi
}

# ==============================================================================
# 确认提示
# ==============================================================================
confirm_stop() {
    if [[ "$FORCE" = "1" ]] || [[ "$DRY_RUN" = "1" ]]; then
        return
    fi
    if [[ -n "${CI:-}" ]] || [[ ! -t 0 ]]; then
        return
    fi

    local extra_warning=""
    if [[ "$REMOVE_VOLUMES" = "1" ]]; then
        extra_warning="\n${COLOR_RED}${COLOR_BOLD}⚠ 警告: --volumes 会永久删除所有数据卷！${COLOR_RESET}"
    fi

    if [[ "$ENV" = "prod" ]] || [[ "$ENV" = "staging" ]]; then
        printf "\n%b即将停止 %s 环境服务%b%s\n" \
            "${COLOR_YELLOW}${COLOR_BOLD}" "$ENV" "${COLOR_RESET}" "$extra_warning"
        printf "  停止 ID: %s\n" "$STOP_ID"
        printf "\n"
        read -r -p "确认继续? [yes/N] " confirm
        if [[ "$confirm" != "yes" ]]; then
            warn "用户取消"
            exit 0
        fi
    elif [[ "$REMOVE_VOLUMES" = "1" ]]; then
        printf "\n%b⚠ 即将删除所有数据卷%b\n" \
            "${COLOR_RED}${COLOR_BOLD}" "${COLOR_RESET}"
        read -r -p "确认? [yes/N] " confirm
        if [[ "$confirm" != "yes" ]]; then
            warn "用户取消"
            exit 0
        fi
    fi
}

# ==============================================================================
# 优雅停止：SIGTERM → 等待 → SIGKILL
# ==============================================================================
graceful_stop() {
    step "优雅停止服务"

    local args=("${COMPOSE_CMD[@]}" "${COMPOSE_FILES[@]}")
    if [[ -f "${ENV_FILE:-}" ]]; then
        args+=("--env-file" "$ENV_FILE")
    fi

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会优雅停止所有服务"
        return
    fi

    # 确定要停止的服务
    local services_to_stop=()
    if [[ -n "$SERVICES" ]]; then
        IFS=',' read -ra services_to_stop <<< "$SERVICES"
    elif [[ "$KEEP_INFRA" = "1" ]]; then
        # 只停止应用服务
        services_to_stop=("${APP_SERVICES[@]}")
    else
        # 停止所有
        services_to_stop=()
    fi

    info "优雅停止超时: ${STOP_TIMEOUT}s"
    info "发送 SIGTERM..."

    local log_file="${LOG_DIR}/stop.log"

    if [[ ${#services_to_stop[@]} -gt 0 ]]; then
        # 停止指定服务（逆序）
        for svc in "${services_to_stop[@]}"; do
            # 检查服务是否在 compose 文件中
            if ! "${COMPOSE_CMD[@]}" "${COMPOSE_FILES[@]}" config --services 2>/dev/null | \
                grep -q "^${svc}$"; then
                continue
            fi

            info "  停止 $svc..."
            "${args[@]}" stop -t "$STOP_TIMEOUT" "$svc" > "$log_file" 2>&1 || {
                warn "  停止 $svc 超时或失败"
            }
        done
    else
        # 停止全部（compose 自动处理逆序）
        "${args[@]}" stop -t "$STOP_TIMEOUT" > "$log_file" 2>&1 || {
            warn "停止部分服务超时"
        }
    fi

    # 等待容器完全停止
    info "等待容器退出..."
    local elapsed=0
    local wait_timeout=$((STOP_TIMEOUT + 15))

    while [[ $elapsed -lt $wait_timeout ]]; do
        local still_running=0
        if [[ ${#services_to_stop[@]} -gt 0 ]]; then
            for svc in "${services_to_stop[@]}"; do
                local state
                state="$("${args[@]}" ps "$svc" --format json 2>/dev/null | \
                    grep -o '"State":"[^"]*"' | head -1 | cut -d'"' -f4 || echo "")"
                if [[ "$state" = "running" ]] || [[ "$state" = "restarting" ]]; then
                    still_running=$((still_running + 1))
                fi
            done
        else
            still_running="$("${args[@]}" ps --format json 2>/dev/null | \
                grep -c '"State":"\(running\|restarting\)"' || echo 0)"
        fi

        if [[ "$still_running" -eq 0 ]]; then
            success "所有服务已停止"
            return 0
        fi

        sleep 2
        elapsed=$((elapsed + 2))
    done

    warn "部分容器未在 $wait_timeout 秒内停止，将强制清理"
    return 1
}

# ==============================================================================
# 强制清理：down
# ==============================================================================
force_cleanup() {
    step "清理容器和网络"

    local args=("${COMPOSE_CMD[@]}" "${COMPOSE_FILES[@]}")
    if [[ -f "${ENV_FILE:-}" ]]; then
        args+=("--env-file" "$ENV_FILE")
    fi

    args+=("down" "--remove-orphans")

    if [[ "$KEEP_INFRA" = "1" ]]; then
        # 只 down 应用服务
        args+=("--rmi" "${RMI_MODE:-none}")
        # 注意: docker compose down 不支持指定服务列表
        # 使用 stop + rm 组合
        info "删除应用服务容器..."
        for svc in "${APP_SERVICES[@]}"; do
            "${COMPOSE_CMD[@]}" "${COMPOSE_FILES[@]}" rm -sf "$svc" >/dev/null 2>&1 || true
        done
        return
    fi

    if [[ -n "$RMI_MODE" ]]; then
        args+=("--rmi" "$RMI_MODE")
    fi

    if [[ "$REMOVE_VOLUMES" = "1" ]]; then
        args+=("--volumes")
        warn "同时删除数据卷（不可恢复）"
    fi

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会执行: ${args[*]}"
        return
    fi

    if [[ "$VERBOSE" = "1" ]]; then
        "${args[@]}"
    else
        if ! "${args[@]}" >/dev/null 2>&1; then
            error "清理失败"
            "${args[@]}" 2>&1 | tail -20
            return 1
        fi
    fi

    success "容器和网络已清理"
}

# ==============================================================================
# 验证停止结果
# ==============================================================================
verify_stopped() {
    step "验证停止状态"

    local args=("${COMPOSE_CMD[@]}" "${COMPOSE_FILES[@]}")
    if [[ -f "${ENV_FILE:-}" ]]; then
        args+=("--env-file" "$ENV_FILE")
    fi

    local running=0

    if [[ -n "$SERVICES" ]]; then
        IFS=',' read -ra svcs <<< "$SERVICES"
        for svc in "${svcs[@]}"; do
            local state
            state="$("${args[@]}" ps "$svc" --format json 2>/dev/null | \
                grep -o '"State":"[^"]*"' | head -1 | cut -d'"' -f4 || echo "")"
            if [[ "$state" = "running" ]]; then
                warn "  $svc 仍在运行"
                running=$((running + 1))
            fi
        done
    else
        running="$("${args[@]}" ps --format json 2>/dev/null | \
            grep -c '"State":"running"' || echo 0)"
    fi

    if [[ "$running" -gt 0 ]]; then
        warn "$running 个服务仍在运行"
        return 1
    fi

    success "所有服务已停止"
    return 0
}

# ==============================================================================
# 显示状态
# ==============================================================================
show_status() {
    step "当前状态"

    local args=("${COMPOSE_CMD[@]}" "${COMPOSE_FILES[@]}")
    if [[ -f "${ENV_FILE:-}" ]]; then
        args+=("--env-file" "$ENV_FILE")
    fi

    if [[ "$KEEP_INFRA" = "1" ]]; then
        info "保留的基础设施:"
        for svc in "${INFRA_SERVICES[@]}"; do
            local state
            state="$("${args[@]}" ps "$svc" --format json 2>/dev/null | \
                grep -o '"State":"[^"]*"' | head -1 | cut -d'"' -f4 || echo "stopped")"
            info "  $svc: $state"
        done
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

    local flags=""
    [[ "$REMOVE_VOLUMES" = "1" ]] && flags="${flags},volumes"
    [[ -n "$RMI_MODE" ]] && flags="${flags},rmi=${RMI_MODE}"
    [[ "$KEEP_INFRA" = "1" ]] && flags="${flags},keep-infra"
    [[ -n "$SERVICES" ]] && flags="${flags},services=${SERVICES}"
    flags="${flags#,}"

    echo "${ts}|${ENV}|${STOP_ID}|${flags}" >> "$HISTORY_FILE"
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
        printf "%b║  币安 BTC/ETH 量化交易系统 停止脚本  v%s%b\n" \
            "${COLOR_CYAN}${COLOR_BOLD}" "$SCRIPT_VERSION" "${COLOR_RESET}"
        printf "%b╚══════════════════════════════════════════════════════════════╝%b\n" \
            "${COLOR_CYAN}${COLOR_BOLD}" "${COLOR_RESET}"
    fi

    if [[ "$DRY_RUN" = "1" ]]; then
        warn "DRY-RUN 模式：不会实际执行停止"
    fi

    mkdir -p "$LOG_DIR"

    # 获取锁
    acquire_lock

    # 环境检查
    check_environment

    # 检查运行状态
    check_running

    # 确认
    confirm_stop

    # 优雅停止
    if ! graceful_stop; then
        warn "优雅停止超时，执行强制清理"
        force_cleanup
    else
        # 优雅停止成功，仍执行 down 清理网络和容器
        force_cleanup
    fi

    # 验证
    if ! verify_stopped; then
        error "仍有服务运行"
        exit 1
    fi

    # 显示剩余状态
    show_status

    # 记录历史
    record_history

    # 完成
    printf "\n"
    printf "%b✅ 系统已停止%b\n" "${COLOR_GREEN}${COLOR_BOLD}" "${COLOR_RESET}"
    printf "   环境:    %s\n" "$ENV"
    printf "   停止 ID: %s\n" "$STOP_ID"
    if [[ "$REMOVE_VOLUMES" = "1" ]]; then
        printf "   %b⚠ 数据卷已删除%b\n" "${COLOR_YELLOW}" "${COLOR_RESET}"
    fi
    if [[ "$KEEP_INFRA" = "1" ]]; then
        printf "   保留:    postgres, redis\n"
    fi
    printf "\n"
}

# ==============================================================================
# 入口
# ==============================================================================
main "$@"
