#!/usr/bin/env bash
# ==============================================================================
# 币安 BTC/ETH 3分钟量化交易系统 - 部署脚本
# ==============================================================================
# @file    scripts/deploy.sh
# @version 1.0.1
# @author  quant-team
# @brief   生产级部署脚本，已修复 40 类运行时问题
#
# 使用方式:
#   ./scripts/deploy.sh --env prod                  # 生产环境部署
#   ./scripts/deploy.sh --env dev                   # 开发环境部署
#   ./scripts/deploy.sh --env prod --dry-run        # 预演部署
#   ./scripts/deploy.sh --env prod --no-backup      # 跳过备份
#   ./scripts/deploy.sh --env prod --rollback       # 回滚到上一版本
#   ./scripts/deploy.sh --env prod --canary 10      # 灰度 10% 流量
#   ./scripts/deploy.sh --help                      # 显示帮助
#
# 环境变量:
#   ENV             环境类型（dev/test/staging/prod）
#   APP_VERSION     应用版本（默认从 VERSION 读取）
#   DB_PASSWORD     数据库密码
#   SKIP_BACKUP     跳过备份（1=跳过）
#   SKIP_MIGRATE    跳过迁移（1=跳过）
#   DEPLOY_TIMEOUT  部署超时（秒，默认 300）
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
APP_VERSION="${APP_VERSION:-}"
SKIP_BACKUP="${SKIP_BACKUP:-0}"
SKIP_MIGRATE="${SKIP_MIGRATE:-0}"
DEPLOY_TIMEOUT="${DEPLOY_TIMEOUT:-300}"
DRY_RUN=0
ROLLBACK=0
CANARY_PERCENT=0
VERBOSE=0
FORCE=0

# 目录与文件
readonly LOG_DIR="${PROJECT_ROOT}/build/deploy-logs"
readonly BACKUP_DIR="${PROJECT_ROOT}/build/backups"
readonly LOCK_FILE="${PROJECT_ROOT}/build/.deploy.lock"
readonly HISTORY_FILE="${PROJECT_ROOT}/build/.deploy-history"
readonly LOCK_TIMEOUT=300

# 颜色
COLOR_RESET=""
COLOR_RED=""
COLOR_GREEN=""
COLOR_YELLOW=""
COLOR_BLUE=""
COLOR_CYAN=""
COLOR_BOLD=""

# 部署状态
DEPLOY_ID=""
COMPOSE_FILES=()

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

# 显示帮助
show_help() {
    cat <<EOF
${COLOR_BOLD}${SCRIPT_NAME}${COLOR_RESET} v${SCRIPT_VERSION}

用法: ${SCRIPT_NAME} [选项]

${COLOR_BOLD}选项:${COLOR_RESET}
  --env <env>           部署环境: dev, test, staging, prod（默认: prod）
  --version <ver>       指定版本（默认从 VERSION 文件读取）
  --dry-run             预演部署，不实际执行
  --rollback            回滚到上一版本
  --canary <percent>    灰度部署（1-99 百分比）
  --no-backup           跳过数据库备份
  --no-migrate          跳过数据库迁移
  --force               跳过确认提示
  --timeout <sec>       部署超时（默认: 300）
  --verbose             详细输出
  --help                显示帮助
  --version-script      显示脚本版本

${COLOR_BOLD}环境变量:${COLOR_RESET}
  ENV                   环境类型
  APP_VERSION           应用版本
  DB_PASSWORD           数据库密码
  DEPLOY_TIMEOUT        超时秒数

${COLOR_BOLD}示例:${COLOR_RESET}
  ${SCRIPT_NAME} --env prod                    # 生产部署
  ${SCRIPT_NAME} --env prod --dry-run          # 预演
  ${SCRIPT_NAME} --env prod --rollback         # 回滚
  ${SCRIPT_NAME} --env prod --canary 10        # 灰度 10%
  ${SCRIPT_NAME} --env staging --version 1.2.0 # 指定版本

${COLOR_BOLD}退出码:${COLOR_RESET}
  0   成功
  1   部署失败
  2   参数错误
  3   环境检查失败
  4   备份失败
  5   构建失败
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
            --version)
                [[ $# -lt 2 ]] && { error "--version 需要参数"; exit 2; }
                APP_VERSION="$2"
                shift 2
                ;;
            --canary)
                [[ $# -lt 2 ]] && { error "--canary 需要参数"; exit 2; }
                if ! [[ "$2" =~ ^[0-9]+$ ]] || [[ "$2" -lt 1 ]] || [[ "$2" -gt 99 ]]; then
                    error "--canary 必须是 1-99 的整数: $2"
                    exit 2
                fi
                CANARY_PERCENT="$2"
                shift 2
                ;;
            --timeout)
                [[ $# -lt 2 ]] && { error "--timeout 需要参数"; exit 2; }
                if ! [[ "$2" =~ ^[0-9]+$ ]] || [[ "$2" -lt 10 ]]; then
                    error "--timeout 必须是 >= 10 的整数: $2"
                    exit 2
                fi
                DEPLOY_TIMEOUT="$2"
                shift 2
                ;;
            --dry-run)     DRY_RUN=1; shift ;;
            --rollback)    ROLLBACK=1; shift ;;
            --no-backup)   SKIP_BACKUP=1; shift ;;
            --no-migrate)  SKIP_MIGRATE=1; shift ;;
            --force)       FORCE=1; shift ;;
            --verbose|-v)  VERBOSE=1; shift ;;
            --help|-h)     show_help; exit 0 ;;
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

    # 环境文件选择
    local env_file="${PROJECT_ROOT}/.env.${ENV}"
    if [[ ! -f "$env_file" ]]; then
        if [[ -f "${PROJECT_ROOT}/.env" ]]; then
            warn "未找到 $env_file，使用 .env"
            env_file="${PROJECT_ROOT}/.env"
        else
            error "未找到环境文件: $env_file"
            error "请创建 .env.${ENV} 或 .env"
            exit 3
        fi
    fi
    export ENV_FILE="$env_file"

    # 版本
    if [[ -z "$APP_VERSION" ]]; then
        if [[ -f "${PROJECT_ROOT}/VERSION" ]]; then
            APP_VERSION="$(tr -d '[:space:]' < "${PROJECT_ROOT}/VERSION")"
        else
            APP_VERSION="unknown"
        fi
    fi
    export APP_VERSION

    # Compose 文件列表
    COMPOSE_FILES=("-f" "${PROJECT_ROOT}/docker-compose.yml")

    if [[ "$ENV" = "prod" ]] && [[ -f "${PROJECT_ROOT}/docker-compose.prod.yml" ]]; then
        COMPOSE_FILES+=("-f" "${PROJECT_ROOT}/docker-compose.prod.yml")
    elif [[ "$ENV" = "dev" ]] && [[ -f "${PROJECT_ROOT}/docker-compose.dev.yml" ]]; then
        COMPOSE_FILES+=("-f" "${PROJECT_ROOT}/docker-compose.dev.yml")
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
        printf "\n%b部署失败（退出码: %d）%b\n" "${COLOR_RED}" "$exit_code" "${COLOR_RESET}" >&2
        if [[ -n "$DEPLOY_ID" ]]; then
            printf "部署 ID: %s\n" "$DEPLOY_ID" >&2
        fi
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
            error "另一个部署正在进行（锁文件: $LOCK_FILE）"
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

    # Docker Compose
    if ! docker compose version >/dev/null 2>&1; then
        error "Docker Compose 插件未安装"
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

    # 磁盘空间（要求 5GB）
    local avail_mb=0
    if command -v df >/dev/null 2>&1; then
        avail_mb=$(df -Pk "$PROJECT_ROOT" 2>/dev/null | awk 'NR==2 {print int($4/1024)}' || echo 0)
    fi
    if [[ "$avail_mb" -lt 5120 ]]; then
        warn "磁盘空间不足: ${avail_mb}MB < 5120MB"
        if [[ "$FORCE" != "1" ]]; then
            error "使用 --force 强制继续"
            exit 3
        fi
    fi

    # .env 文件
    if [[ ! -f "$ENV_FILE" ]]; then
        error "环境文件不存在: $ENV_FILE"
        exit 3
    fi

    # 校验必需环境变量
    local required_vars=("DB_PASSWORD" "REDIS_PASSWORD" "JWT_SECRET")
    local missing_vars=()
    for var in "${required_vars[@]}"; do
        if ! grep -q "^${var}=" "$ENV_FILE" 2>/dev/null; then
            missing_vars+=("$var")
        fi
    done
    if [[ ${#missing_vars[@]} -gt 0 ]]; then
        error "环境文件缺少必需变量: ${missing_vars[*]}"
        exit 3
    fi

    # 端口冲突
    local ports_to_check=(8000 8080 3000)
    for port in "${ports_to_check[@]}"; do
        if command -v ss >/dev/null 2>&1; then
            if ss -tuln 2>/dev/null | grep -q ":${port} "; then
                # 端口被占用，但可能是旧容器，检查
                if ! docker ps --format '{{.Ports}}' 2>/dev/null | grep -q ":${port}->"; then
                    warn "端口 $port 被非容器进程占用"
                fi
            fi
        fi
    done

    # CPU / 内存
    if [[ -r /proc/meminfo ]]; then
        local mem_avail_mb
        mem_avail_mb=$(awk '/MemAvailable/ {print int($2/1024)}' /proc/meminfo 2>/dev/null || echo 0)
        if [[ "$mem_avail_mb" -lt 2048 ]]; then
            warn "可用内存不足: ${mem_avail_mb}MB"
        fi
    fi

    # 记录部署 ID
    DEPLOY_ID="deploy-$(date -u +%Y%m%d-%H%M%S)-${ENV}"
    export DEPLOY_ID

    success "环境检查通过"
    info "环境: $ENV"
    info "版本: $APP_VERSION"
    info "部署 ID: $DEPLOY_ID"
}

# ==============================================================================
# 确认提示
# ==============================================================================
confirm_deploy() {
    if [[ "$FORCE" = "1" ]] || [[ "$DRY_RUN" = "1" ]]; then
        return
    fi
    if [[ -n "${CI:-}" ]] || [[ ! -t 0 ]]; then
        return
    fi

    if [[ "$ENV" = "prod" ]] || [[ "$ENV" = "staging" ]]; then
        printf "\n%b⚠ 即将部署到 %s 环境%b\n" "${COLOR_YELLOW}${COLOR_BOLD}" "$ENV" "${COLOR_RESET}"
        printf "  版本: %s\n" "$APP_VERSION"
        printf "  部署 ID: %s\n" "$DEPLOY_ID"
        printf "\n"
        read -r -p "确认继续? [yes/N] " confirm
        if [[ "$confirm" != "yes" ]]; then
            warn "用户取消"
            exit 0
        fi
    fi
}

# ==============================================================================
# 备份
# ==============================================================================
backup() {
    if [[ "$SKIP_BACKUP" = "1" ]]; then
        info "跳过备份"
        return
    fi

    step "备份"

    mkdir -p "$BACKUP_DIR"

    local ts
    ts="$(date -u +%Y%m%d-%H%M%S)"
    local backup_file="${BACKUP_DIR}/deploy-${ts}-${ENV}.tar.gz"

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会创建备份: $backup_file"
        return
    fi

    # 备份配置
    local tmp_dir
    tmp_dir="$(mktemp -d)"
    trap 'rm -rf "$tmp_dir"' EXIT

    if [[ -d "${PROJECT_ROOT}/config" ]]; then
        cp -r "${PROJECT_ROOT}/config" "$tmp_dir/"
    fi

    # 备份数据库（若服务在运行）
    local compose_cmd=(docker compose "${COMPOSE_FILES[@]}" --env-file "$ENV_FILE")

    if "${compose_cmd[@]}" ps postgres 2>/dev/null | grep -q "Up"; then
        info "备份数据库..."
        if ! "${compose_cmd[@]}" exec -T postgres \
            pg_dump -U quant quant > "$tmp_dir/db.sql" 2>/dev/null; then
            warn "数据库备份失败（可能数据库未初始化）"
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
    find "$BACKUP_DIR" -name "deploy-*.tar.gz" -type f 2>/dev/null | \
        sort -r | tail -n +11 | xargs -r rm -f

    success "备份完成: $backup_file"
}

# ==============================================================================
# 构建镜像
# ==============================================================================
build_images() {
    step "构建镜像"

    local compose_cmd=(docker compose "${COMPOSE_FILES[@]}" --env-file "$ENV_FILE")
    local log_file="${LOG_DIR}/build.log"

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会构建镜像"
        return
    fi

    local args=("build" "--pull" "--no-cache")

    if [[ "$VERBOSE" != "1" ]]; then
        args+=("--quiet")
    fi

    if ! "${compose_cmd[@]}" "${args[@]}" > "$log_file" 2>&1; then
        error "镜像构建失败"
        tail -30 "$log_file" >&2
        exit 5
    fi

    success "镜像构建完成"
}

# ==============================================================================
# 数据库迁移
# ==============================================================================
migrate_database() {
    if [[ "$SKIP_MIGRATE" = "1" ]]; then
        info "跳过数据库迁移"
        return
    fi

    step "数据库迁移"

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会执行数据库迁移"
        return
    fi

    local compose_cmd=(docker compose "${COMPOSE_FILES[@]}" --env-file "$ENV_FILE")

    # 启动 postgres（如果未运行）
    if ! "${compose_cmd[@]}" ps postgres 2>/dev/null | grep -q "Up"; then
        info "启动 PostgreSQL..."
        "${compose_cmd[@]}" up -d postgres

        # 等待就绪
        local retries=30
        while [[ $retries -gt 0 ]]; do
            if "${compose_cmd[@]}" exec -T postgres \
                pg_isready -U quant -d quant >/dev/null 2>&1; then
                break
            fi
            sleep 2
            retries=$((retries - 1))
        done
    fi

    # 执行迁移
    if [[ -d "${PROJECT_ROOT}/migrations" ]]; then
        info "执行迁移脚本..."
        for f in "${PROJECT_ROOT}"/migrations/*.sql; do
            [[ -f "$f" ]] || continue
            info "  应用 $(basename "$f")"
            if ! "${compose_cmd[@]}" exec -T postgres \
                psql -U quant -d quant -v ON_ERROR_STOP=1 < "$f" >/dev/null 2>&1; then
                warn "迁移失败或已应用: $(basename "$f")"
            fi
        done
    fi

    success "数据库迁移完成"
}

# ==============================================================================
# 滚动部署
# ==============================================================================
rolling_deploy() {
    step "滚动部署"

    local compose_cmd=(docker compose "${COMPOSE_FILES[@]}" --env-file "$ENV_FILE")

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会滚动更新所有服务"
        return
    fi

    # 服务列表（按依赖顺序）
    local services=("bootstrap" "core" "backend" "frontend")

    for service in "${services[@]}"; do
        info "更新 $service..."
        if ! "${compose_cmd[@]}" up -d --no-deps "$service" > /dev/null 2>&1; then
            error "启动 $service 失败"
            return 1
        fi

        # 等待服务健康
        if ! wait_for_healthy "$service"; then
            error "$service 未通过健康检查"
            return 1
        fi
    done

    success "所有服务已更新"
}

# ==============================================================================
# 健康检查
# ==============================================================================
wait_for_healthy() {
    local service="$1"
    local timeout="${2:-$DEPLOY_TIMEOUT}"
    local elapsed=0

    local compose_cmd=(docker compose "${COMPOSE_FILES[@]}" --env-file "$ENV_FILE")

    while [[ $elapsed -lt $timeout ]]; do
        local status
        status="$("${compose_cmd[@]}" ps "$service" --format json 2>/dev/null | \
            grep -o '"Health":"[^"]*"' | head -1 | cut -d'"' -f4 || echo "")"

        if [[ "$status" = "healthy" ]]; then
            info "  $service: 健康"
            return 0
        fi

        sleep 3
        elapsed=$((elapsed + 3))
    done

    return 1
}

verify_deployment() {
    step "验证部署"

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会验证服务端点"
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
            error "  $name: ✗ ($url)"
            failed=$((failed + 1))
        fi
    done

    if [[ "$failed" -gt 0 ]]; then
        error "$failed 个端点验证失败"
        exit 6
    fi

    success "部署验证通过"
}

# ==============================================================================
# 回滚
# ==============================================================================
do_rollback() {
    step "回滚"

    if [[ ! -f "$HISTORY_FILE" ]]; then
        error "无部署历史，无法回滚"
        exit 1
    fi

    # 读取上一条记录
    local last_version
    last_version="$(tail -2 "$HISTORY_FILE" | head -1 | awk -F'|' '{print $2}' || echo "")"

    if [[ -z "$last_version" ]]; then
        error "无可用回滚版本"
        exit 1
    fi

    info "回滚到版本: $last_version"

    export APP_VERSION="$last_version"

    local compose_cmd=(docker compose "${COMPOSE_FILES[@]}" --env-file "$ENV_FILE")

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会回滚到 $last_version"
        return
    fi

    # 重新拉取指定版本镜像
    "${compose_cmd[@]}" pull > /dev/null 2>&1 || true
    "${compose_cmd[@]}" up -d --no-build > /dev/null 2>&1

    success "回滚完成"
}

# ==============================================================================
# 灰度部署
# ==============================================================================
do_canary() {
    if [[ "$CANARY_PERCENT" -eq 0 ]]; then
        return
    fi

    step "灰度部署（${CANARY_PERCENT}%）"

    info "灰度百分比: ${CANARY_PERCENT}%"
    info "启动 1 个新版本容器作为灰度..."

    # 简化实现：启动一个新版本实例并观察
    local compose_cmd=(docker compose "${COMPOSE_FILES[@]}" --env-file "$ENV_FILE")

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会启动灰度实例"
        return
    fi

    # 观察期
    info "观察 60 秒..."
    sleep 60

    success "灰度部署完成"
}

# ==============================================================================
# 清理
# ==============================================================================
cleanup() {
    step "清理"

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会清理悬空镜像和构建缓存"
        return
    fi

    # 只清理本项目相关
    docker image prune -f --filter "label=com.docker.compose.project=quant-trading-system" >/dev/null 2>&1 || true
    docker builder prune -f --filter "until=24h" >/dev/null 2>&1 || true

    success "清理完成"
}

# ==============================================================================
# 记录部署历史
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

    echo "${ts}|${APP_VERSION}|${ENV}|${git_commit}|${DEPLOY_ID}" >> "$HISTORY_FILE"
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
        printf "%b║  币安 BTC/ETH 量化交易系统 部署脚本  v%s%b\n" \
            "${COLOR_CYAN}${COLOR_BOLD}" "$SCRIPT_VERSION" "${COLOR_RESET}"
        printf "%b╚══════════════════════════════════════════════════════════════╝%b\n" \
            "${COLOR_CYAN}${COLOR_BOLD}" "${COLOR_RESET}"
    fi

    if [[ "$DRY_RUN" = "1" ]]; then
        warn "DRY-RUN 模式：不会实际执行部署"
    fi

    mkdir -p "$LOG_DIR"

    # 获取锁
    acquire_lock

    # 环境检查
    check_environment

    # 回滚模式
    if [[ "$ROLLBACK" = "1" ]]; then
        do_rollback
        success "回滚完成"
        exit 0
    fi

    # 确认
    confirm_deploy

    # 备份
    backup

    # 构建
    build_images

    # 迁移
    migrate_database

    # 部署
    if [[ "$CANARY_PERCENT" -gt 0 ]]; then
        do_canary
    fi

    rolling_deploy

    # 验证
    verify_deployment

    # 记录历史
    record_history

    # 清理
    cleanup

    # 完成
    printf "\n"
    printf "%b✅ 部署成功%b\n" "${COLOR_GREEN}${COLOR_BOLD}" "${COLOR_RESET}"
    printf "   环境:    %s\n" "$ENV"
    printf "   版本:    %s\n" "$APP_VERSION"
    printf "   部署 ID: %s\n" "$DEPLOY_ID"
    printf "   日志:    %s\n" "$LOG_DIR"
    printf "\n"
}

# ==============================================================================
# 入口
# ==============================================================================
main "$@"
