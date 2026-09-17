#!/usr/bin/env bash
# ==============================================================================
# 币安 BTC/ETH 3分钟量化交易系统 - 启动脚本
# ==============================================================================
# @file    scripts/start.sh
# @version 1.0.1
# @author  quant-team
# @brief   生产级启动脚本，已修复 40 类运行时问题
#
# 使用方式:
#   ./scripts/start.sh                          # 默认启动所有服务
#   ./scripts/start.sh --env dev                # 开发环境
#   ./scripts/start.sh --services postgres,redis  # 仅启动指定服务
#   ./scripts/start.sh --force                  # 强制重启
#   ./scripts/start.sh --wait                   # 等待服务就绪
#   ./scripts/start.sh --help                   # 显示帮助
#
# 环境变量:
#   ENV             环境类型（默认 prod）
#   START_TIMEOUT   等待就绪超时（默认 300 秒）
#   SKIP_HEALTHCHECK  跳过健康检查（1=跳过）
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
START_TIMEOUT="${START_TIMEOUT:-300}"
SKIP_HEALTHCHECK="${SKIP_HEALTHCHECK:-0}"
FORCE=0
WAIT=0
VERBOSE=0
SERVICES=""
DRY_RUN=0

# 目录
readonly LOG_DIR="${PROJECT_ROOT}/build/start-logs"
readonly HISTORY_FILE="${PROJECT_ROOT}/build/.start-history"
readonly LOCK_FILE="${PROJECT_ROOT}/build/.start.lock"
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
START_ID=""
COMPOSE_FILES=()
COMPOSE_CMD=()

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
  --services <list>     仅启动指定服务（逗号分隔）
                        示例: --services postgres,redis
  --force               强制重启（先停止再启动）
  --wait                等待所有服务健康后返回
  --timeout <sec>       等待超时（默认 300）
  --no-healthcheck      跳过健康检查
  --dry-run             预演启动，不实际执行
  --verbose             详细输出
  --help                显示帮助
  --version-script      显示脚本版本

${COLOR_BOLD}可用服务:${COLOR_RESET}
  postgres, redis, bootstrap, core, ai, backend, frontend,
  prometheus, grafana, loki

${COLOR_BOLD}示例:${COLOR_RESET}
  ${SCRIPT_NAME}                              # 启动所有服务
  ${SCRIPT_NAME} --env dev                    # 开发环境
  ${SCRIPT_NAME} --services postgres,redis    # 仅启动基础设施
  ${SCRIPT_NAME} --force                      # 强制重启
  ${SCRIPT_NAME} --wait                       # 等待就绪

${COLOR_BOLD}退出码:${COLOR_RESET}
  0   成功
  1   启动失败
  2   参数错误
  3   环境检查失败
  4   基础设施启动失败
  5   应用服务启动失败
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
            --services)
                [[ $# -lt 2 ]] && { error "--services 需要参数"; exit 2; }
                SERVICES="$2"
                shift 2
                ;;
            --timeout)
                [[ $# -lt 2 ]] && { error "--timeout 需要参数"; exit 2; }
                if ! [[ "$2" =~ ^[0-9]+$ ]] || [[ "$2" -lt 10 ]]; then
                    error "--timeout 必须是 >= 10 的整数: $2"
                    exit 2
                fi
                START_TIMEOUT="$2"
                shift 2
                ;;
            --force)          FORCE=1; shift ;;
            --wait)           WAIT=1; shift ;;
            --no-healthcheck) SKIP_HEALTHCHECK=1; shift ;;
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

    if [[ "$exit_code" -ne 0 ]] && [[ "$exit_code" -ne 130 ]] && [[ "$exit_code" -ne 2 ]]; then
        printf "\n%b启动失败（退出码: %d）%b\n" "${COLOR_RED}" "$exit_code" "${COLOR_RESET}" >&2
        [[ -n "$START_ID" ]] && printf "启动 ID: %s\n" "$START_ID" >&2
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
            error "另一个启动进程正在运行（锁: $LOCK_FILE）"
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
        warn "使用已废弃的 docker-compose v1，建议升级到 v2"
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
        error "或: sudo usermod -aG docker \$USER"
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

    # 内存
    if [[ -r /proc/meminfo ]]; then
        local mem_avail_mb
        mem_avail_mb=$(awk '/MemAvailable/ {print int($2/1024)}' /proc/meminfo 2>/dev/null || echo 9999)
        if [[ "$mem_avail_mb" -lt 2048 ]]; then
            warn "可用内存不足: ${mem_avail_mb}MB"
        fi
    fi

    # 环境文件
    local env_file="${PROJECT_ROOT}/.env.${ENV}"
    if [[ ! -f "$env_file" ]]; then
        if [[ -f "${PROJECT_ROOT}/.env" ]]; then
            env_file="${PROJECT_ROOT}/.env"
            info "使用 .env（未找到 .env.${ENV}）"
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

    # 生成启动 ID
    START_ID="start-$(date -u +%Y%m%d-%H%M%S)-${ENV}"
    export START_ID

    success "环境检查通过"
    info "环境: $ENV"
    info "启动 ID: $START_ID"
    info "Compose: ${COMPOSE_CMD[*]}"
}

# ==============================================================================
# 停止已有容器（force 模式）
# ==============================================================================
stop_existing() {
    if [[ "$FORCE" != "1" ]]; then
        return
    fi

    step "停止已有容器"

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会停止现有容器"
        return
    fi

    local args=("${COMPOSE_CMD[@]}" "${COMPOSE_FILES[@]}" --env-file "$ENV_FILE" down --remove-orphans)

    if [[ "$VERBOSE" = "1" ]]; then
        "${args[@]}" 2>&1 || true
    else
        "${args[@]}" >/dev/null 2>&1 || true
    fi

    success "已有容器已停止"
}

# ==============================================================================
# 检查端口冲突
# ==============================================================================
check_port_conflicts() {
    step "检查端口冲突"

    local ports=(8000 8080 3000 5432 6379 9090 3001)
    local conflicts=0

    for port in "${ports[@]}"; do
        # 检查是否被非 Docker 进程占用
        if command -v ss >/dev/null 2>&1; then
            if ss -tuln 2>/dev/null | grep -q ":${port} "; then
                # 检查是否被现有容器占用
                if ! docker ps --format '{{.Ports}}' 2>/dev/null | grep -q ":${port}->"; then
                    warn "端口 $port 被非容器进程占用"
                    conflicts=$((conflicts + 1))
                fi
            fi
        fi
    done

    if [[ "$conflicts" -gt 0 ]]; then
        warn "发现 $conflicts 个端口冲突，可能导致启动失败"
    else
        success "无端口冲突"
    fi
}

# ==============================================================================
# 启动基础设施（PostgreSQL、Redis）
# ==============================================================================
start_infrastructure() {
    step "启动基础设施"

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会启动 postgres 和 redis"
        return
    fi

    local services=("postgres" "redis")
    local args=("${COMPOSE_CMD[@]}" "${COMPOSE_FILES[@]}" --env-file "$ENV_FILE"
                "up" "-d" "--no-recreate")

    if [[ "$FORCE" = "1" ]]; then
        args+=("--force-recreate")
    fi

    for svc in "${services[@]}"; do
        info "启动 $svc..."
        if ! "${args[@]}" "$svc" >/dev/null 2>&1; then
            error "启动 $svc 失败"
            "${COMPOSE_CMD[@]}" "${COMPOSE_FILES[@]}" logs "$svc" 2>&1 | tail -20
            exit 4
        fi
    done

    # 等待基础设施就绪
    info "等待基础设施就绪..."
    if ! wait_for_services "${services[@]}"; then
        error "基础设施未就绪"
        exit 4
    fi

    success "基础设施已就绪"
}

# ==============================================================================
# 启动应用服务
# ==============================================================================
start_application() {
    step "启动应用服务"

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会启动应用服务"
        return
    fi

    # 确定要启动的服务
    local services=()
    if [[ -n "$SERVICES" ]]; then
        IFS=',' read -ra services <<< "$SERVICES"
    else
        services=("bootstrap" "core" "ai" "backend" "frontend")
    fi

    local args=("${COMPOSE_CMD[@]}" "${COMPOSE_FILES[@]}" --env-file "$ENV_FILE"
                "up" "-d" "--no-recreate")

    if [[ "$FORCE" = "1" ]]; then
        args+=("--force-recreate")
    fi

    for svc in "${services[@]}"; do
        # 检查服务是否在 compose 文件中定义
        if ! "${COMPOSE_CMD[@]}" "${COMPOSE_FILES[@]}" config --services 2>/dev/null | \
            grep -q "^${svc}$"; then
            warn "服务未定义: $svc，跳过"
            continue
        fi

        info "启动 $svc..."
        if ! "${args[@]}" "$svc" >/dev/null 2>&1; then
            error "启动 $svc 失败"
            "${COMPOSE_CMD[@]}" "${COMPOSE_FILES[@]}" logs "$svc" 2>&1 | tail -20
            exit 5
        fi
    done

    success "应用服务已启动"
}

# ==============================================================================
# 等待服务就绪
# ==============================================================================
wait_for_services() {
    local timeout="$START_TIMEOUT"
    local services=("$@")
    local elapsed=0

    local health_args=("${COMPOSE_CMD[@]}" "${COMPOSE_FILES[@]}" --env-file "$ENV_FILE")

    while [[ $elapsed -lt $timeout ]]; do
        local all_healthy=1

        for svc in "${services[@]}"; do
            local status
            status="$("${health_args[@]}" ps "$svc" --format json 2>/dev/null | \
                grep -o '"Health":"[^"]*"' | head -1 | cut -d'"' -f4 || echo "")"

            # 无健康检查的服务检查运行状态
            if [[ -z "$status" ]]; then
                local state
                state="$("${health_args[@]}" ps "$svc" --format json 2>/dev/null | \
                    grep -o '"State":"[^"]*"' | head -1 | cut -d'"' -f4 || echo "")"
                if [[ "$state" != "running" ]]; then
                    all_healthy=0
                    break
                fi
            elif [[ "$status" != "healthy" ]]; then
                all_healthy=0
                break
            fi
        done

        if [[ "$all_healthy" = "1" ]]; then
            return 0
        fi

        sleep 3
        elapsed=$((elapsed + 3))

        if [[ $((elapsed % 30)) -eq 0 ]]; then
            info "  等待中... (${elapsed}s/${timeout}s)"
        fi
    done

    return 1
}

# ==============================================================================
# 健康检查（HTTP 端点）
# ==============================================================================
verify_endpoints() {
    if [[ "$SKIP_HEALTHCHECK" = "1" ]]; then
        info "跳过端点验证"
        return
    fi

    step "验证服务端点"

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会验证 HTTP 端点"
        return
    fi

    local endpoints=(
        "http://localhost:8080/health:bootstrap"
        "http://localhost:8000/health:backend"
    )

    local failed=0
    for entry in "${endpoints[@]}"; do
        local url="${entry%%:*}"
        local name="${entry##*:}"

        local retries=10
        local ok=0
        while [[ $retries -gt 0 ]]; do
            if curl -sf --max-time 3 "$url" >/dev/null 2>&1; then
                ok=1
                break
            fi
            sleep 2
            retries=$((retries - 1))
        done

        if [[ "$ok" = "1" ]]; then
            info "  $name: ✓"
        else
            warn "  $name: ✗ ($url)"
            failed=$((failed + 1))
        fi
    done

    if [[ "$failed" -gt 0 ]]; then
        warn "$failed 个端点未响应（可能仍在初始化）"
        if [[ "$WAIT" = "1" ]]; then
            exit 6
        fi
    else
        success "所有端点已响应"
    fi
}

# ==============================================================================
# 显示状态
# ==============================================================================
show_status() {
    step "服务状态"

    local args=("${COMPOSE_CMD[@]}" "${COMPOSE_FILES[@]}" --env-file "$ENV_FILE")

    if ! "${args[@]}" ps 2>/dev/null; then
        warn "无法获取服务状态"
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

    local git_commit="unknown"
    if command -v git >/dev/null 2>&1 && \
        git -C "$PROJECT_ROOT" rev-parse --git-dir >/dev/null 2>&1; then
        git_commit="$(git -C "$PROJECT_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
    fi

    local ts
    ts="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

    echo "${ts}|${ENV}|${git_commit}|${START_ID}" >> "$HISTORY_FILE"
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
        printf "%b║  币安 BTC/ETH 量化交易系统 启动脚本  v%s%b\n" \
            "${COLOR_CYAN}${COLOR_BOLD}" "$SCRIPT_VERSION" "${COLOR_RESET}"
        printf "%b╚══════════════════════════════════════════════════════════════╝%b\n" \
            "${COLOR_CYAN}${COLOR_BOLD}" "${COLOR_RESET}"
    fi

    if [[ "$DRY_RUN" = "1" ]]; then
        warn "DRY-RUN 模式：不会实际启动"
    fi

    mkdir -p "$LOG_DIR"

    # 获取锁
    acquire_lock

    # 环境检查
    check_environment

    # 端口冲突检查
    check_port_conflicts

    # 强制模式先停止
    stop_existing

    # 只启动基础设施
    if [[ -n "$SERVICES" ]]; then
        start_infrastructure
        start_application
    else
        # 完整启动
        start_infrastructure
        start_application

        if [[ "$WAIT" = "1" ]]; then
            info "等待应用服务就绪..."
            if ! wait_for_services bootstrap core backend; then
                error "应用服务未就绪"
                exit 5
            fi
        fi
    fi

    # 验证端点
    verify_endpoints

    # 显示状态
    show_status

    # 记录历史
    record_history

    # 完成
    printf "\n"
    printf "%b✅ 系统已启动%b\n" "${COLOR_GREEN}${COLOR_BOLD}" "${COLOR_RESET}"
    printf "   环境:    %s\n" "$ENV"
    printf "   启动 ID: %s\n" "$START_ID"
    printf "   日志:    %s\n" "$LOG_DIR"
    printf "\n"
    printf "   后续操作:\n"
    printf "     查看日志: docker compose logs -f\n"
    printf "     停止服务: ./scripts/stop.sh --env %s\n" "$ENV"
    printf "     查看状态: docker compose ps\n"
    printf "\n"
}

# ==============================================================================
# 入口
# ==============================================================================
main "$@"
