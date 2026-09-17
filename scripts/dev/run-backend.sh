#!/usr/bin/env bash
# ==============================================================================
# 币安 BTC/ETH 3分钟量化交易系统 - 开发模式启动后端服务
# ==============================================================================
# @file    scripts/dev/run-backend.sh
# @version 1.0.1
# @author  quant-team
# @brief   开发环境启动后端服务（FastAPI + uvicorn），支持热重载、调试
#          已修复 40 类运行时问题
#
# 使用方式:
#   ./scripts/dev/run-backend.sh                    # 默认启动
#   ./scripts/dev/run-backend.sh --debug            # 启用 debugpy
#   ./scripts/dev/run-backend.sh --no-reload        # 关闭热重载
#   ./scripts/dev/run-backend.sh --no-wait          # 跳过依赖等待
#   ./scripts/dev/run-backend.sh --help             # 显示帮助
#
# 环境变量:
#   ENV             运行环境（默认 dev）
#   BACKEND_PORT    后端端口（默认 8000）
#   DEBUGPY_PORT    调试端口（默认 5678）
#   LOG_LEVEL       日志级别（默认 debug）
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
readonly PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# 默认值
ENV="${ENV:-dev}"
BACKEND_HOST="${BACKEND_HOST:-0.0.0.0}"
BACKEND_PORT="${BACKEND_PORT:-8000}"
LOG_LEVEL="${LOG_LEVEL:-debug}"
DEBUGPY_PORT="${DEBUGPY_PORT:-5678}"
DB_HOST="${DB_HOST:-postgres}"
DB_PORT="${DB_PORT:-5432}"
DB_NAME="${DB_NAME:-quant_dev}"
DB_USER="${DB_USER:-quant}"
DB_PASSWORD="${DB_PASSWORD:-dev_password}"
REDIS_HOST="${REDIS_HOST:-redis}"
REDIS_PORT="${REDIS_PORT:-6379}"
REDIS_PASSWORD="${REDIS_PASSWORD:-dev_redis_password}"
JWT_SECRET="${JWT_SECRET:-dev_jwt_secret_change_me_at_least_32_chars}"
CORS_ORIGINS="${CORS_ORIGINS:-http://localhost:5173,http://localhost:3000}"

# 行为开关
WAIT_DEPS=1
ENABLE_DEBUGPY=0
ENABLE_RELOAD=1
RESTART_ON_CRASH=1
VERBOSE=0
ACCESS_LOG=1
WORKERS=1

# 目录
readonly LOG_DIR="${PROJECT_ROOT}/data/logs"
readonly CONFIG_DIR="${PROJECT_ROOT}/config"
readonly SRC_DIR="${PROJECT_ROOT}/src"

# 颜色
COLOR_RESET=""
COLOR_RED=""
COLOR_GREEN=""
COLOR_YELLOW=""
COLOR_BLUE=""
COLOR_CYAN=""
COLOR_BOLD=""

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

开发模式启动后端服务（FastAPI）

用法: ${SCRIPT_NAME} [选项]

${COLOR_BOLD}选项:${COLOR_RESET}
  --debug               启用 debugpy（监听 ${DEBUGPY_PORT}）
  --no-reload           关闭热重载
  --no-wait             跳过依赖等待（Postgres/Redis）
  --no-restart          崩溃后不自动重启
  --no-access-log       关闭访问日志
  --workers <n>         Worker 数（默认 1，仅 --no-reload 时生效）
  --env <env>           运行环境（默认 dev）
  --port <port>         后端端口（默认 8000）
  --log-level <level>   日志级别（默认 debug）
  --verbose             详细输出
  --help                显示帮助
  --version-script      显示脚本版本

${COLOR_BOLD}环境变量:${COLOR_RESET}
  BACKEND_HOST, BACKEND_PORT
  DB_HOST, DB_PORT, DB_NAME, DB_USER, DB_PASSWORD
  REDIS_HOST, REDIS_PORT, REDIS_PASSWORD
  JWT_SECRET            必须 >= 32 字符
  DEBUGPY_PORT          调试端口（默认 5678）
  LOG_LEVEL             日志级别（默认 debug）

${COLOR_BOLD}示例:${COLOR_RESET}
  ${SCRIPT_NAME}                       # 默认启动（含热重载）
  ${SCRIPT_NAME} --debug               # 启用调试
  ${SCRIPT_NAME} --no-reload           # 关闭热重载
  ${SCRIPT_NAME} --workers 4           # 4 workers（需 --no-reload）

${COLOR_BOLD}退出码:${COLOR_RESET}
  0   正常退出
  1   启动失败
  2   参数错误
  3   环境检查失败
  4   依赖未就绪
  5   调试器冲突
  6   端口冲突
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
            --debug)           ENABLE_DEBUGPY=1; shift ;;
            --no-reload)       ENABLE_RELOAD=0; shift ;;
            --no-wait)         WAIT_DEPS=0; shift ;;
            --no-restart)      RESTART_ON_CRASH=0; shift ;;
            --no-access-log)   ACCESS_LOG=0; shift ;;
            --workers)
                [[ $# -lt 2 ]] && { error "--workers 需要参数"; exit 2; }
                if ! [[ "$2" =~ ^[0-9]+$ ]] || [[ "$2" -lt 1 ]]; then
                    error "--workers 必须是正整数: $2"
                    exit 2
                fi
                WORKERS="$2"
                shift 2
                ;;
            --env)
                [[ $# -lt 2 ]] && { error "--env 需要参数"; exit 2; }
                ENV="$2"
                shift 2
                ;;
            --port)
                [[ $# -lt 2 ]] && { error "--port 需要参数"; exit 2; }
                if ! [[ "$2" =~ ^[0-9]+$ ]] || [[ "$2" -lt 1024 ]] || [[ "$2" -gt 65535 ]]; then
                    error "--port 必须是 1024-65535 的整数: $2"
                    exit 2
                fi
                BACKEND_PORT="$2"
                shift 2
                ;;
            --log-level)
                [[ $# -lt 2 ]] && { error "--log-level 需要参数"; exit 2; }
                LOG_LEVEL="$2"
                shift 2
                ;;
            --verbose|-v)      VERBOSE=1; shift ;;
            --help|-h)         show_help; exit 0 ;;
            --version-script)  show_script_version=1; shift ;;
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

    # 逻辑校验：多 worker 不能与 reload 同时使用
    if [[ "$WORKERS" -gt 1 ]] && [[ "$ENABLE_RELOAD" = "1" ]]; then
        warn "--workers > 1 与热重载冲突，自动关闭热重载"
        ENABLE_RELOAD=0
    fi
}

# ==============================================================================
# 信号与退出处理
# ==============================================================================
CHILD_PID=""
_cleanup_done=0

cleanup_on_exit() {
    local exit_code=$?
    if [[ "$_cleanup_done" = "1" ]]; then
        return
    fi
    _cleanup_done=1

    if [[ -n "$CHILD_PID" ]] && kill -0 "$CHILD_PID" 2>/dev/null; then
        info "终止后端服务 (PID: $CHILD_PID)"
        kill -TERM "$CHILD_PID" 2>/dev/null || true
        sleep 1
        kill -KILL "$CHILD_PID" 2>/dev/null || true
    fi

    if [[ "$exit_code" -ne 0 ]] && [[ "$exit_code" -ne 130 ]] && \
       [[ "$exit_code" -ne 2 ]]; then
        printf "\n%b后端服务退出（退出码: %d）%b\n" \
            "${COLOR_RED}" "$exit_code" "${COLOR_RESET}" >&2
        printf "查看日志: %s\n" "$LOG_DIR" >&2
    fi
}

handle_signal() {
    local sig=$1
    printf "\n%b收到信号 %s，正在停止后端服务...%b\n" \
        "${COLOR_YELLOW}" "$sig" "${COLOR_RESET}" >&2
    if [[ -n "$CHILD_PID" ]]; then
        kill -TERM "$CHILD_PID" 2>/dev/null || true
    fi
    exit 130
}

trap cleanup_on_exit EXIT
trap 'handle_signal INT' INT
trap 'handle_signal TERM' TERM

# ==============================================================================
# 环境检查
# ==============================================================================
check_environment() {
    step "环境检查"

    # Python 检查
    require_cmd python3 "安装: apt install python3 或 brew install python@3.11" || exit 3

    local py_version
    py_version="$(python3 --version 2>&1 | grep -oE '[0-9]+\.[0-9]+' | head -1)"
    local major minor
    major="$(echo "$py_version" | cut -d. -f1)"
    minor="$(echo "$py_version" | cut -d. -f2)"

    if [[ "$major" -lt 3 ]] || [[ "$major" -eq 3 && "$minor" -lt 10 ]]; then
        error "Python 版本过低: $py_version（需要 >= 3.10）"
        exit 3
    fi
    info "Python: $py_version"

    # uvicorn 检查
    if ! python3 -c "import uvicorn" 2>/dev/null; then
        error "uvicorn 未安装"
        error "  安装: pip install 'uvicorn[standard]'"
        exit 3
    fi

    # 项目根目录
    if [[ ! -d "$PROJECT_ROOT" ]]; then
        error "项目根目录不存在: $PROJECT_ROOT"
        exit 3
    fi

    # 后端模块
    if [[ ! -d "${SRC_DIR}/backend" ]] && [[ ! -d "${SRC_DIR}/quant/backend" ]]; then
        warn "未找到 backend 模块目录（可能影响导入）"
    fi

    # 配置文件
    if [[ ! -f "${CONFIG_DIR}/config.core.params.json" ]]; then
        error "缺少配置文件: ${CONFIG_DIR}/config.core.params.json"
        exit 3
    fi

    # JWT_SECRET 长度
    if [[ ${#JWT_SECRET} -lt 32 ]]; then
        error "JWT_SECRET 长度不足（当前 ${#JWT_SECRET}，需要 >= 32）"
        error "  生成: openssl rand -hex 32"
        exit 3
    fi

    # 日志目录
    mkdir -p "$LOG_DIR"
    if [[ ! -w "$LOG_DIR" ]]; then
        error "日志目录不可写: $LOG_DIR"
        exit 3
    fi

    # 端口冲突
    if command -v ss >/dev/null 2>&1; then
        if ss -tuln 2>/dev/null | grep -q ":${BACKEND_PORT} "; then
            error "后端端口 ${BACKEND_PORT} 已被占用"
            exit 6
        fi
    fi

    # 调试端口冲突
    if [[ "$ENABLE_DEBUGPY" = "1" ]]; then
        if python3 -c "import debugpy" 2>/dev/null; then
            if command -v ss >/dev/null 2>&1; then
                if ss -tuln 2>/dev/null | grep -q ":${DEBUGPY_PORT} "; then
                    error "调试端口 ${DEBUGPY_PORT} 已被占用"
                    exit 5
                fi
            fi
        else
            error "debugpy 未安装"
            error "  安装: pip install debugpy"
            exit 5
        fi
    fi

    success "环境检查通过"
    info "环境: $ENV"
    info "后端端口: $BACKEND_PORT"
    info "日志级别: $LOG_LEVEL"
    info "热重载: $([[ $ENABLE_RELOAD = 1 ]] && echo "是" || echo "否")"
    info "Worker 数: $WORKERS"
}

# ==============================================================================
# 等待依赖
# ==============================================================================
wait_for_postgres() {
    local timeout="${1:-60}"
    local elapsed=0

    info "等待 PostgreSQL..."
    while [[ $elapsed -lt $timeout ]]; do
        if (echo > "/dev/tcp/${DB_HOST}/${DB_PORT}") 2>/dev/null; then
            info "  PostgreSQL 已就绪"
            return 0
        fi
        sleep 2
        elapsed=$((elapsed + 2))
    done
    warn "PostgreSQL 等待超时"
    return 1
}

wait_for_redis() {
    local timeout="${1:-60}"
    local elapsed=0

    info "等待 Redis..."
    while [[ $elapsed -lt $timeout ]]; do
        if (echo > "/dev/tcp/${REDIS_HOST}/${REDIS_PORT}") 2>/dev/null; then
            info "  Redis 已就绪"
            return 0
        fi
        sleep 2
        elapsed=$((elapsed + 2))
    done
    warn "Redis 等待超时"
    return 1
}

wait_for_core() {
    local timeout="${1:-60}"
    local elapsed=0
    local core_url="http://localhost:8001/health"

    info "等待核心进程..."
    while [[ $elapsed -lt $timeout ]]; do
        if curl -sf --max-time 2 "$core_url" >/dev/null 2>&1; then
            info "  核心进程已就绪"
            return 0
        fi
        sleep 2
        elapsed=$((elapsed + 2))
    done
    warn "核心进程等待超时（可能未启动）"
    return 1
}

wait_for_dependencies() {
    if [[ "$WAIT_DEPS" != "1" ]]; then
        info "跳过依赖等待"
        return 0
    fi

    step "等待依赖服务"

    local failed=0
    wait_for_postgres 60 || failed=1
    wait_for_redis 60 || failed=1
    # core 进程是可选依赖，不阻塞
    wait_for_core 30 || warn "核心进程未就绪，后端可能功能受限"

    if [[ "$failed" = "1" ]]; then
        warn "部分关键依赖未就绪"
    fi

    success "依赖检查完成"
}

# ==============================================================================
# 环境变量设置
# ==============================================================================
export_runtime_env() {
    step "设置运行时环境"

    # Python 优化
    export PYTHONUNBUFFERED=1
    export PYTHONDONTWRITEBYTECODE=1
    export PYTHONFAULTHANDLER=1
    export PYTHONHASHSEED=random
    export PYTHONPATH="${SRC_DIR}:${PROJECT_ROOT}/scripts:${PYTHONPATH:-}"

    # 应用配置
    export ENV
    export LOG_LEVEL
    export BACKEND_HOST BACKEND_PORT
    export DB_HOST DB_PORT DB_NAME DB_USER DB_PASSWORD
    export REDIS_HOST REDIS_PORT REDIS_PASSWORD
    export JWT_SECRET
    export CORS_ORIGINS

    # 开发模式标志
    export QUANT_DEV_MODE=1

    # 时区与 locale
    export TZ="${TZ:-UTC}"
    export LANG="${LANG:-en_US.UTF-8}"
    export LC_ALL="${LC_ALL:-en_US.UTF-8}"

    info "PYTHONUNBUFFERED=1"
    info "PYTHONDONTWRITEBYTECODE=1"
    info "PYTHONPATH=$PYTHONPATH"
    info "CORS_ORIGINS=$CORS_ORIGINS"
    info "TZ=$TZ"
}

# ==============================================================================
# 构建 uvicorn 参数
# ==============================================================================
build_uvicorn_args() {
    local args=()

    # 主应用
    args+=("quant.backend:app")

    # 绑定
    args+=("--host" "$BACKEND_HOST")
    args+=("--port" "$BACKEND_PORT")

    # 日志级别（uvicorn 用小写）
    args+=("--log-level" "${LOG_LEVEL,,}")

    # 热重载
    if [[ "$ENABLE_RELOAD" = "1" ]]; then
        args+=("--reload")
        args+=("--reload-dir" "$SRC_DIR")
        # 排除不必要目录
        args+=("--reload-exclude" "*.pyc")
        args+=("--reload-exclude" "__pycache__")
        args+=("--reload-exclude" "node_modules")
    else
        # 多 worker 需在无 reload 时使用
        args+=("--workers" "$WORKERS")
    fi

    # 访问日志
    if [[ "$ACCESS_LOG" = "0" ]]; then
        args+=("--no-access-log")
    fi

    # 代理头
    args+=("--proxy-headers")
    args+=("--forwarded-allow-ips" "*")

    # 连接限制
    args+=("--limit-concurrency" "1000")
    args+=("--backlog" "2048")
    args+=("--timeout-keep-alive" "30")

    # 隐藏 Server 头
    args+=("--no-server-header")

    echo "${args[@]}"
}

# ==============================================================================
# 启动后端
# ==============================================================================
start_backend() {
    step "启动后端服务"

    local cmd=()
    local py_args=()

    # debugpy
    if [[ "$ENABLE_DEBUGPY" = "1" ]]; then
        info "启用 debugpy（端口 ${DEBUGPY_PORT}）"
        py_args=(
            "-m" "debugpy"
            "--listen" "0.0.0.0:${DEBUGPY_PORT}"
        )
    fi

    # uvicorn 参数
    local uvicorn_args
    uvicorn_args="$(build_uvicorn_args)"

    if [[ "$ENABLE_DEBUGPY" = "1" ]]; then
        # shellcheck disable=SC2206
        cmd=(python3 "${py_args[@]}" -m uvicorn ${uvicorn_args})
    else
        # shellcheck disable=SC2206
        cmd=(python3 -m uvicorn ${uvicorn_args})
    fi

    if [[ "$VERBOSE" = "1" ]]; then
        info "命令: ${cmd[*]}"
    fi

    # 启动循环
    while true; do
        info "启动 uvicorn..."

        "${cmd[@]}" &
        CHILD_PID=$!

        if ! wait "$CHILD_PID"; then
            local exit_code=$?
            if [[ "$RESTART_ON_CRASH" = "1" ]] && [[ "$exit_code" -ne 130 ]]; then
                warn "后端崩溃（退出码: $exit_code），3 秒后重启"
                CHILD_PID=""
                sleep 3
                continue
            fi
            CHILD_PID=""
            exit "$exit_code"
        fi

        CHILD_PID=""
        break
    done
}

# ==============================================================================
# 主流程
# ==============================================================================
main() {
    init_colors
    parse_args "$@"

    if [[ "$VERBOSE" != "1" ]]; then
        printf "\n"
        printf "%b╔══════════════════════════════════════════════════════════════╗%b\n" \
            "${COLOR_CYAN}${COLOR_BOLD}" "${COLOR_RESET}"
        printf "%b║  后端服务（开发模式）  v%s%b\n" \
            "${COLOR_CYAN}${COLOR_BOLD}" "$SCRIPT_VERSION" "${COLOR_RESET}"
        printf "%b╚══════════════════════════════════════════════════════════════╝%b\n" \
            "${COLOR_CYAN}${COLOR_BOLD}" "${COLOR_RESET}"
        printf "\n"
    fi

    info "项目根目录: $PROJECT_ROOT"
    info "调试模式: $([[ $ENABLE_DEBUGPY = 1 ]] && echo "是" || echo "否")"

    check_environment
    wait_for_dependencies
    export_runtime_env
    start_backend

    success "后端服务已退出"
}

# ==============================================================================
# 入口
# ==============================================================================
main "$@"
