#!/usr/bin/env bash
# ==============================================================================
# 币安 BTC/ETH 3分钟量化交易系统 - 开发模式启动 Bootstrap 协调服务
# ==============================================================================
# @file    scripts/dev/run-bootstrap.sh
# @version 1.0.1
# @author  quant-team
# @brief   开发环境启动 Bootstrap 服务（C++ 二进制），支持调试、依赖等待
#          已修复 40 类运行时问题
#
# 使用方式:
#   ./scripts/dev/run-bootstrap.sh                 # 默认启动
#   ./scripts/dev/run-bootstrap.sh --debug         # 启用 gdbserver
#   ./scripts/dev/run-bootstrap.sh --asan          # 使用 ASan 构建
#   ./scripts/dev/run-bootstrap.sh --no-wait       # 跳过依赖等待
#   ./scripts/dev/run-bootstrap.sh --help          # 显示帮助
#
# 环境变量:
#   ENV             运行环境（默认 dev）
#   BOOTSTRAP_PORT  Bootstrap 端口（默认 8080）
#   GDB_PORT        gdbserver 端口（默认 2345）
#   LOG_LEVEL       日志级别（默认 DEBUG）
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
BOOTSTRAP_PORT="${BOOTSTRAP_PORT:-8080}"
GDB_PORT="${GDB_PORT:-2345}"
LOG_LEVEL="${LOG_LEVEL:-DEBUG}"
DB_HOST="${DB_HOST:-postgres}"
DB_PORT="${DB_PORT:-5432}"
DB_NAME="${DB_NAME:-quant_dev}"
DB_USER="${DB_USER:-quant}"
DB_PASSWORD="${DB_PASSWORD:-dev_password}"
REDIS_HOST="${REDIS_HOST:-redis}"
REDIS_PORT="${REDIS_PORT:-6379}"
REDIS_PASSWORD="${REDIS_PASSWORD:-dev_redis_password}"

# 行为开关
WAIT_DEPS=1
ENABLE_DEBUG=0
ENABLE_ASAN=0
ENABLE_VALGRIND=0
RESTART_ON_CRASH=1
VERBOSE=0

# 目录
readonly LOG_DIR="${PROJECT_ROOT}/data/logs"
readonly CONFIG_DIR="${PROJECT_ROOT}/config"
readonly BUILD_DIR="${PROJECT_ROOT}/build"
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

开发模式启动 Bootstrap 协调服务（C++ 二进制）

用法: ${SCRIPT_NAME} [选项]

${COLOR_BOLD}选项:${COLOR_RESET}
  --debug               启用 gdbserver（监听 ${GDB_PORT}）
  --asan                使用 ASan 构建的二进制
  --valgrind            使用 valgrind 运行
  --no-wait             跳过依赖等待（Postgres/Redis）
  --no-restart          崩溃后不自动重启
  --env <env>           运行环境（默认 dev）
  --port <port>         Bootstrap 端口（默认 8080）
  --log-level <level>   日志级别（默认 DEBUG）
  --verbose             详细输出
  --help                显示帮助
  --version-script      显示脚本版本

${COLOR_BOLD}环境变量:${COLOR_RESET}
  DB_HOST, DB_PORT, DB_NAME, DB_USER, DB_PASSWORD
  REDIS_HOST, REDIS_PORT, REDIS_PASSWORD
  BOOTSTRAP_PORT        Bootstrap 端口（默认 8080）
  GDB_PORT              gdbserver 端口（默认 2345）
  LOG_LEVEL             日志级别（默认 DEBUG）

${COLOR_BOLD}示例:${COLOR_RESET}
  ${SCRIPT_NAME}                       # 默认启动
  ${SCRIPT_NAME} --debug               # gdbserver 调试
  ${SCRIPT_NAME} --asan                # ASan 构建
  ${SCRIPT_NAME} --valgrind            # 内存检查

${COLOR_BOLD}退出码:${COLOR_RESET}
  0   正常退出
  1   启动失败
  2   参数错误
  3   环境检查失败
  4   依赖未就绪
  5   调试器冲突
  6   端口冲突
  7   二进制缺失
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
            --debug)           ENABLE_DEBUG=1; shift ;;
            --asan)            ENABLE_ASAN=1; shift ;;
            --valgrind)        ENABLE_VALGRIND=1; shift ;;
            --no-wait)         WAIT_DEPS=0; shift ;;
            --no-restart)      RESTART_ON_CRASH=0; shift ;;
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
                BOOTSTRAP_PORT="$2"
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

    # 校验环境
    case "$ENV" in
        dev|test|staging|prod) ;;
        *)
            error "无效环境: $ENV"
            exit 2
            ;;
    esac

    # 互斥检查
    if [[ "$ENABLE_DEBUG" = "1" ]] && [[ "$ENABLE_VALGRIND" = "1" ]]; then
        error "--debug 与 --valgrind 不能同时使用"
        exit 2
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
        info "终止 Bootstrap 进程 (PID: $CHILD_PID)"
        kill -TERM "$CHILD_PID" 2>/dev/null || true
        sleep 1
        kill -KILL "$CHILD_PID" 2>/dev/null || true
    fi

    if [[ "$exit_code" -ne 0 ]] && [[ "$exit_code" -ne 130 ]] && \
       [[ "$exit_code" -ne 2 ]]; then
        printf "\n%bBootstrap 服务退出（退出码: %d）%b\n" \
            "${COLOR_RED}" "$exit_code" "${COLOR_RESET}" >&2
        printf "查看日志: %s\n" "$LOG_DIR" >&2
    fi
}

handle_signal() {
    local sig=$1
    printf "\n%b收到信号 %s，正在停止 Bootstrap...%b\n" \
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
# 定位二进制
# ==============================================================================
locate_binary() {
    local candidates=()

    if [[ "$ENABLE_ASAN" = "1" ]]; then
        candidates+=(
            "${BUILD_DIR}/debug/bin/quant_bootstrap"
            "${BUILD_DIR}/asan/bin/quant_bootstrap"
        )
    else
        candidates+=(
            "${BUILD_DIR}/debug/bin/quant_bootstrap"
            "${BUILD_DIR}/release/bin/quant_bootstrap"
        )
    fi

    for candidate in "${candidates[@]}"; do
        if [[ -x "$candidate" ]]; then
            echo "$candidate"
            return 0
        fi
    done

    return 1
}

# ==============================================================================
# 环境检查
# ==============================================================================
check_environment() {
    step "环境检查"

    # 项目根目录
    if [[ ! -d "$PROJECT_ROOT" ]]; then
        error "项目根目录不存在: $PROJECT_ROOT"
        exit 3
    fi

    # 二进制
    local binary
    if ! binary="$(locate_binary)"; then
        error "未找到 quant_bootstrap 二进制"
        error "  期望路径: ${BUILD_DIR}/debug/bin/quant_bootstrap"
        error "  请先构建: ./scripts/build.sh --type Debug"
        exit 7
    fi
    info "二进制: $binary"
    info "大小: $(du -h "$binary" | cut -f1)"

    # 可执行权限
    if [[ ! -x "$binary" ]]; then
        error "二进制不可执行: $binary"
        error "  尝试: chmod +x $binary"
        exit 7
    fi

    # 架构匹配
    if command -v file >/dev/null 2>&1; then
        local file_type
        file_type="$(file -b "$binary")"
        if ! echo "$file_type" | grep -qE "ELF|Mach-O|PE32"; then
            warn "二进制类型不明确: $file_type"
        fi
    fi

    # 配置文件
    if [[ ! -f "${CONFIG_DIR}/config.core.params.json" ]]; then
        error "缺少配置文件: ${CONFIG_DIR}/config.core.params.json"
        exit 3
    fi

    # 环境文件
    local env_file="${PROJECT_ROOT}/.env.${ENV}"
    if [[ ! -f "$env_file" ]] && [[ ! -f "${PROJECT_ROOT}/.env" ]]; then
        warn "未找到 .env.${ENV} 或 .env，使用默认值"
    fi

    # 日志目录
    mkdir -p "$LOG_DIR"
    if [[ ! -w "$LOG_DIR" ]]; then
        error "日志目录不可写: $LOG_DIR"
        exit 3
    fi

    # 数据目录
    mkdir -p "${PROJECT_ROOT}/data/snapshots" \
             "${PROJECT_ROOT}/data/replay" \
             "${PROJECT_ROOT}/data/cache"

    # 端口冲突
    if command -v ss >/dev/null 2>&1; then
        if ss -tuln 2>/dev/null | grep -q ":${BOOTSTRAP_PORT} "; then
            error "Bootstrap 端口 ${BOOTSTRAP_PORT} 已被占用"
            exit 6
        fi
    fi

    # 调试端口
    if [[ "$ENABLE_DEBUG" = "1" ]]; then
        require_cmd gdbserver "安装: apt install gdbserver" || exit 5

        if command -v ss >/dev/null 2>&1; then
            if ss -tuln 2>/dev/null | grep -q ":${GDB_PORT} "; then
                error "gdbserver 端口 ${GDB_PORT} 已被占用"
                exit 5
            fi
        fi
    fi

    # valgrind
    if [[ "$ENABLE_VALGRIND" = "1" ]]; then
        require_cmd valgrind "安装: apt install valgrind" || exit 5
    fi

    # ulimit
    local nofile_limit
    nofile_limit="$(ulimit -n 2>/dev/null || echo 0)"
    if [[ "$nofile_limit" -lt 4096 ]]; then
        warn "文件描述符限制过低: $nofile_limit（建议 >= 4096）"
        ulimit -n 65536 2>/dev/null || true
    fi

    success "环境检查通过"
    info "环境: $ENV"
    info "端口: $BOOTSTRAP_PORT"
    info "日志级别: $LOG_LEVEL"
    info "调试: $([[ $ENABLE_DEBUG = 1 ]] && echo "gdbserver:$GDB_PORT" || echo "否")"
    info "ASan: $([[ $ENABLE_ASAN = 1 ]] && echo "是" || echo "否")"

    # 返回二进制路径
    echo "$binary"
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

wait_for_dependencies() {
    if [[ "$WAIT_DEPS" != "1" ]]; then
        info "跳过依赖等待"
        return 0
    fi

    step "等待依赖服务"

    local failed=0
    wait_for_postgres 60 || failed=1
    wait_for_redis 60 || failed=1

    if [[ "$failed" = "1" ]]; then
        warn "部分依赖未就绪，Bootstrap 可能启动失败"
    fi

    success "依赖检查完成"
}

# ==============================================================================
# 数据库迁移
# ==============================================================================
run_migrations() {
    if [[ "$WAIT_DEPS" != "1" ]]; then
        return 0
    fi

    local migrations_dir="${PROJECT_ROOT}/migrations"
    if [[ ! -d "$migrations_dir" ]]; then
        return 0
    fi

    if ! command -v psql >/dev/null 2>&1; then
        info "未找到 psql，跳过迁移"
        return 0
    fi

    step "运行数据库迁移"

    local applied=0
    for f in "${migrations_dir}"/*.sql; do
        [[ -f "$f" ]] || continue
        local name
        name="$(basename "$f")"

        if PGPASSWORD="$DB_PASSWORD" psql \
            -h "$DB_HOST" -p "$DB_PORT" -U "$DB_USER" -d "$DB_NAME" \
            -v ON_ERROR_STOP=1 -f "$f" >/dev/null 2>&1; then
            info "  ✓ $name"
            applied=$((applied + 1))
        fi
    done

    info "应用 $applied 个迁移"
}

# ==============================================================================
# 环境变量设置
# ==============================================================================
export_runtime_env() {
    step "设置运行时环境"

    # Python 优化（若 Bootstrap 调用 Python 脚本）
    export PYTHONUNBUFFERED=1
    export PYTHONDONTWRITEBYTECODE=1
    export PYTHONFAULTHANDLER=1
    export PYTHONPATH="${SRC_DIR}:${PROJECT_ROOT}/scripts:${PYTHONPATH:-}"

    # 应用配置
    export ENV
    export LOG_LEVEL
    export BOOTSTRAP_PORT
    export DB_HOST DB_PORT DB_NAME DB_USER DB_PASSWORD
    export REDIS_HOST REDIS_PORT REDIS_PASSWORD

    # 时区与 locale
    export TZ="${TZ:-UTC}"
    export LANG="${LANG:-en_US.UTF-8}"
    export LC_ALL="${LC_ALL:-en_US.UTF-8}"

    # 动态库路径（本地开发可能使用非系统库）
    local lib_paths=()
    [[ -d "${BUILD_DIR}/debug/lib" ]] && lib_paths+=("${BUILD_DIR}/debug/lib")
    [[ -d "${BUILD_DIR}/release/lib" ]] && lib_paths+=("${BUILD_DIR}/release/lib")
    if [[ ${#lib_paths[@]} -gt 0 ]]; then
        local joined
        joined="$(IFS=:; echo "${lib_paths[*]}")"
        export LD_LIBRARY_PATH="${joined}:${LD_LIBRARY_PATH:-}"
        info "LD_LIBRARY_PATH=$LD_LIBRARY_PATH"
    fi

    # 开发模式标志
    export QUANT_DEV_MODE=1
    export QUANT_ENV="$ENV"

    # ASan 选项
    if [[ "$ENABLE_ASAN" = "1" ]]; then
        export ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:abort_on_error=1:print_stacktrace=1"
        export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"
        info "ASAN_OPTIONS=$ASAN_OPTIONS"
    fi

    info "TZ=$TZ"
    info "PYTHONPATH=$PYTHONPATH"
}

# ==============================================================================
# 启动 Bootstrap
# ==============================================================================
start_bootstrap() {
    local binary="$1"

    step "启动 Bootstrap 服务"

    local cmd=()

    if [[ "$ENABLE_DEBUG" = "1" ]]; then
        info "使用 gdbserver（端口 ${GDB_PORT}）"
        cmd=(gdbserver "0.0.0.0:${GDB_PORT}" "$binary")
    elif [[ "$ENABLE_VALGRIND" = "1" ]]; then
        info "使用 valgrind"
        cmd=(
            valgrind
            --leak-check=full
            --track-origins=yes
            --log-file="${LOG_DIR}/valgrind.log"
            "$binary"
        )
    else
        cmd=("$binary")
    fi

    # Bootstrap 参数
    cmd+=("--env" "$ENV")
    cmd+=("--port" "$BOOTSTRAP_PORT")
    cmd+=("--log-level" "$LOG_LEVEL")

    if [[ "$VERBOSE" = "1" ]]; then
        info "命令: ${cmd[*]}"
    fi

    # 运行循环
    while true; do
        info "启动 Bootstrap..."

        "${cmd[@]}" &
        CHILD_PID=$!

        if ! wait "$CHILD_PID"; then
            local exit_code=$?
            if [[ "$RESTART_ON_CRASH" = "1" ]] && [[ "$exit_code" -ne 130 ]] && \
               [[ "$exit_code" -ne 0 ]]; then
                warn "Bootstrap 崩溃（退出码: $exit_code），3 秒后重启"
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
        printf "%b║  Bootstrap 服务（开发模式）  v%s%b\n" \
            "${COLOR_CYAN}${COLOR_BOLD}" "$SCRIPT_VERSION" "${COLOR_RESET}"
        printf "%b╚══════════════════════════════════════════════════════════════╝%b\n" \
            "${COLOR_CYAN}${COLOR_BOLD}" "${COLOR_RESET}"
        printf "\n"
    fi

    info "项目根目录: $PROJECT_ROOT"
    info "调试模式: $([[ $ENABLE_DEBUG = 1 ]] && echo "是" || echo "否")"

    local binary
    binary="$(check_environment)"

    wait_for_dependencies
    run_migrations
    export_runtime_env
    start_bootstrap "$binary"

    success "Bootstrap 服务已退出"
}

# ==============================================================================
# 入口
# ==============================================================================
main "$@"
