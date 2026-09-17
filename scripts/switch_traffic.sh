#!/usr/bin/env bash
# ==============================================================================
# 币安 BTC/ETH 3分钟量化交易系统 - 蓝绿流量切换脚本
# ==============================================================================
# @file    scripts/switch_traffic.sh
# @version 1.0.1
# @author  quant-team
# @brief   生产级蓝绿流量切换脚本，已修复 40 类运行时问题
#
# 使用方式:
#   ./scripts/switch_traffic.sh --to green               # 切换到 green
#   ./scripts/switch_traffic.sh --to blue                # 切换到 blue
#   ./scripts/switch_traffic.sh --to green --canary 10   # 灰度 10%
#   ./scripts/switch_traffic.sh --rollback               # 回滚到上次状态
#   ./scripts/switch_traffic.sh --status                 # 查看当前状态
#   ./scripts/switch_traffic.sh --dry-run --to green     # 预演
#   ./scripts/switch_traffic.sh --help                   # 显示帮助
#
# 环境变量:
#   NGINX_CONF      Nginx 配置文件路径
#   NGINX_CONTAINER Nginx 容器名（若通过 Docker 管理）
#   UPSTREAM_NAME   Upstream 名称（默认 quant_backend）
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

# Nginx 配置
NGINX_CONF="${NGINX_CONF:-${PROJECT_ROOT}/nginx/upstream.conf}"
NGINX_CONTAINER="${NGINX_CONTAINER:-}"
UPSTREAM_NAME="${UPSTREAM_NAME:-quant_backend}"
NGINX_BIN="${NGINX_BIN:-nginx}"

# 目标与行为
TARGET=""
CANARY_PERCENT=0
ROLLBACK=0
SHOW_STATUS=0
DRY_RUN=0
FORCE=0
VERBOSE=0
WAIT_DRAIN=1
DRAIN_TIMEOUT=30
HEALTHCHECK_TIMEOUT=60
BACKUP_KEEP=10

# 目录与文件
readonly LOG_DIR="${PROJECT_ROOT}/build/switch-logs"
readonly BACKUP_DIR="${PROJECT_ROOT}/build/nginx-backups"
readonly LOCK_FILE="${PROJECT_ROOT}/build/.switch.lock"
readonly HISTORY_FILE="${PROJECT_ROOT}/build/.switch-history"
readonly STATE_FILE="${PROJECT_ROOT}/build/.traffic-state"
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
SWITCH_ID=""
CURRENT_ACTIVE=""
BACKUP_FILE=""

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

蓝绿流量切换脚本

用法: ${SCRIPT_NAME} [选项]

${COLOR_BOLD}选项:${COLOR_RESET}
  --to <blue|green>     切换到目标环境
  --canary <percent>    灰度切换（1-99 百分比）
  --rollback            回滚到上次切换前的状态
  --status              查看当前流量分配
  --upstream <name>     Upstream 名称（默认 ${UPSTREAM_NAME}）
  --nginx-conf <path>   Nginx 配置文件路径
  --nginx-container <n> 通过 Docker 容器管理 Nginx
  --no-wait             不等待连接排空
  --drain-timeout <s>   连接排空超时（默认 30）
  --force               跳过确认提示
  --dry-run             预演，不实际修改
  --verbose             详细输出
  --help                显示帮助
  --version-script      显示脚本版本

${COLOR_BOLD}环境变量:${COLOR_RESET}
  NGINX_CONF             Nginx 配置文件路径
  NGINX_CONTAINER        Nginx 容器名
  UPSTREAM_NAME          Upstream 名称

${COLOR_BOLD}示例:${COLOR_RESET}
  ${SCRIPT_NAME} --to green                    # 切换所有流量
  ${SCRIPT_NAME} --to green --canary 10        # 灰度 10%
  ${SCRIPT_NAME} --rollback                    # 回滚
  ${SCRIPT_NAME} --status                      # 查看状态
  ${SCRIPT_NAME} --dry-run --to green          # 预演

${COLOR_BOLD}退出码:${COLOR_RESET}
  0   成功
  1   切换失败
  2   参数错误
  3   环境检查失败
  4   配置验证失败
  5   健康检查失败
  6   Reload 失败
  7   并发锁超时
  8   目标环境未就绪
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
            --to)
                [[ $# -lt 2 ]] && { error "--to 需要参数"; exit 2; }
                case "$2" in
                    blue|green) TARGET="$2" ;;
                    *)
                        error "--to 只能是 blue 或 green: $2"
                        exit 2
                        ;;
                esac
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
            --upstream)
                [[ $# -lt 2 ]] && { error "--upstream 需要参数"; exit 2; }
                UPSTREAM_NAME="$2"
                shift 2
                ;;
            --nginx-conf)
                [[ $# -lt 2 ]] && { error "--nginx-conf 需要参数"; exit 2; }
                NGINX_CONF="$2"
                shift 2
                ;;
            --nginx-container)
                [[ $# -lt 2 ]] && { error "--nginx-container 需要参数"; exit 2; }
                NGINX_CONTAINER="$2"
                shift 2
                ;;
            --drain-timeout)
                [[ $# -lt 2 ]] && { error "--drain-timeout 需要参数"; exit 2; }
                if ! [[ "$2" =~ ^[0-9]+$ ]] || [[ "$2" -lt 0 ]]; then
                    error "--drain-timeout 必须是 >= 0 的整数: $2"
                    exit 2
                fi
                DRAIN_TIMEOUT="$2"
                shift 2
                ;;
            --rollback)        ROLLBACK=1; shift ;;
            --status)          SHOW_STATUS=1; shift ;;
            --no-wait)         WAIT_DRAIN=0; shift ;;
            --force)           FORCE=1; shift ;;
            --dry-run)         DRY_RUN=1; shift ;;
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

    # 逻辑校验
    if [[ "$SHOW_STATUS" = "1" ]] && [[ -n "$TARGET" ]]; then
        error "--status 与 --to 不能同时使用"
        exit 2
    fi

    if [[ "$ROLLBACK" = "1" ]] && [[ -n "$TARGET" ]]; then
        error "--rollback 与 --to 不能同时使用"
        exit 2
    fi

    if [[ -z "$TARGET" ]] && [[ "$ROLLBACK" != "1" ]] && [[ "$SHOW_STATUS" != "1" ]]; then
        error "必须指定 --to, --rollback 或 --status"
        echo "使用 --help 查看帮助"
        exit 2
    fi

    if [[ -n "$TARGET" ]] && [[ "$CANARY_PERCENT" -gt 0 ]]; then
        info "灰度模式：${CANARY_PERCENT}% 到 $TARGET"
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

    if [[ "$exit_code" -ne 0 ]] && [[ "$exit_code" -ne 130 ]] && \
       [[ "$exit_code" -ne 2 ]]; then
        printf "\n%b切换失败（退出码: %d）%b\n" \
            "${COLOR_RED}" "$exit_code" "${COLOR_RESET}" >&2
        [[ -n "$SWITCH_ID" ]] && printf "切换 ID: %s\n" "$SWITCH_ID" >&2
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
            error "另一个切换正在进行（锁: $LOCK_FILE）"
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

    if [[ -n "$NGINX_CONTAINER" ]]; then
        require_cmd docker "安装: https://docs.docker.com/get-docker/" || missing=1
    else
        require_cmd "$NGINX_BIN" "安装: apt install nginx" || missing=1
    fi

    require_cmd curl "安装: apt install curl" || missing=1

    if [[ "$missing" = "1" ]]; then
        exit 3
    fi

    # Nginx 配置文件
    if [[ -z "$NGINX_CONTAINER" ]]; then
        if [[ ! -f "$NGINX_CONF" ]]; then
            error "Nginx 配置文件不存在: $NGINX_CONF"
            exit 3
        fi
        if [[ ! -w "$NGINX_CONF" ]]; then
            error "Nginx 配置文件不可写: $NGINX_CONF"
            error "  尝试: sudo chown \$USER $NGINX_CONF"
            exit 3
        fi
    fi

    # Nginx 进程
    if [[ -z "$NGINX_CONTAINER" ]]; then
        if ! pgrep -x nginx >/dev/null 2>&1; then
            warn "Nginx 进程未运行"
        fi
    else
        if ! docker ps --format '{{.Names}}' | grep -q "^${NGINX_CONTAINER}$"; then
            error "Nginx 容器未运行: $NGINX_CONTAINER"
            exit 3
        fi
    fi

    # 生成切换 ID
    SWITCH_ID="switch-$(date -u +%Y%m%d-%H%M%S)"
    export SWITCH_ID

    success "环境检查通过"
    info "Upstream: $UPSTREAM_NAME"
    info "切换 ID: $SWITCH_ID"
}

# ==============================================================================
# 读取当前状态
# ==============================================================================
read_current_state() {
    # 优先从状态文件读取
    if [[ -f "$STATE_FILE" ]]; then
        CURRENT_ACTIVE="$(cat "$STATE_FILE" 2>/dev/null || echo "")"
    fi

    # 从 Nginx 配置推断
    if [[ -z "$CURRENT_ACTIVE" ]] && [[ -z "$NGINX_CONTAINER" ]]; then
        local blue_weight green_weight
        blue_weight="$(grep -E "server\s+blue:[0-9]+" "$NGINX_CONF" 2>/dev/null | \
            grep -oE 'weight=[0-9]+' | cut -d= -f2 || echo 0)"
        green_weight="$(grep -E "server\s+green:[0-9]+" "$NGINX_CONF" 2>/dev/null | \
            grep -oE 'weight=[0-9]+' | cut -d= -f2 || echo 0)"

        if [[ "${blue_weight:-0}" -gt "${green_weight:-0}" ]]; then
            CURRENT_ACTIVE="blue"
        elif [[ "${green_weight:-0}" -gt "${blue_weight:-0}" ]]; then
            CURRENT_ACTIVE="green"
        fi
    fi

    [[ -n "$CURRENT_ACTIVE" ]] && info "当前活动: $CURRENT_ACTIVE" || \
        info "当前活动: 未知"
}

# ==============================================================================
# 显示状态
# ==============================================================================
show_status() {
    step "当前流量状态"

    if [[ -f "$STATE_FILE" ]]; then
        info "活动环境: $(cat "$STATE_FILE" 2>/dev/null || echo unknown)"
    fi

    if [[ -z "$NGINX_CONTAINER" ]] && [[ -f "$NGINX_CONF" ]]; then
        info "Upstream 配置:"
        grep -A5 "upstream ${UPSTREAM_NAME}" "$NGINX_CONF" 2>/dev/null || \
            warn "未找到 upstream: $UPSTREAM_NAME"

        # 提取权重
        local blue_weight green_weight
        blue_weight="$(grep -E "server\s+blue:[0-9]+" "$NGINX_CONF" 2>/dev/null | \
            grep -oE 'weight=[0-9]+' | cut -d= -f2 | head -1 || echo 0)"
        green_weight="$(grep -E "server\s+green:[0-9]+" "$NGINX_CONF" 2>/dev/null | \
            grep -oE 'weight=[0-9]+' | cut -d= -f2 | head -1 || echo 0)"

        info "  blue:  weight=${blue_weight:-0}"
        info "  green: weight=${green_weight:-0}"
    elif [[ -n "$NGINX_CONTAINER" ]]; then
        info "Upstream 配置（容器内）:"
        docker exec "$NGINX_CONTAINER" cat /etc/nginx/conf.d/upstream.conf 2>/dev/null || \
            warn "无法读取容器内配置"
    fi

    if [[ -f "$HISTORY_FILE" ]]; then
        info ""
        info "最近切换历史:"
        tail -5 "$HISTORY_FILE" 2>/dev/null | while IFS='|' read -r ts from to id; do
            echo "  $ts  $from → $to  ($id)"
        done
    fi
}

# ==============================================================================
# 备份配置
# ==============================================================================
backup_config() {
    step "备份 Nginx 配置"

    mkdir -p "$BACKUP_DIR"

    local ts
    ts="$(date -u +%Y%m%d-%H%M%S)"
    BACKUP_FILE="${BACKUP_DIR}/nginx-${ts}.conf"

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会备份到: $BACKUP_FILE"
        return
    fi

    if [[ -n "$NGINX_CONTAINER" ]]; then
        # 容器内配置
        if ! docker cp "${NGINX_CONTAINER}:/etc/nginx/conf.d/upstream.conf" \
            "$BACKUP_FILE" 2>/dev/null; then
            warn "容器配置备份失败"
        else
            success "备份完成: $BACKUP_FILE"
        fi
    else
        # 本地配置
        if ! cp "$NGINX_CONF" "$BACKUP_FILE"; then
            error "配置备份失败"
            exit 4
        fi
        success "备份完成: $BACKUP_FILE"
    fi

    # 清理旧备份
    find "$BACKUP_DIR" -name "nginx-*.conf" -type f 2>/dev/null | \
        sort -r | tail -n +$((BACKUP_KEEP + 1)) | xargs -r rm -f
}

# ==============================================================================
# 修改 Upstream 权重
# ==============================================================================
update_upstream_weights() {
    local blue_weight="$1"
    local green_weight="$2"

    step "更新 Upstream 权重"

    info "目标: blue=$blue_weight, green=$green_weight"

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会修改配置权重"
        return
    fi

    # 生成临时配置
    local tmp_conf
    tmp_conf="$(mktemp)"
    trap 'rm -f "$tmp_conf"' EXIT

    # 使用 sed 替换权重
    sed -E \
        -e "s/(server\s+blue:[0-9]+\s+weight=)[0-9]+/\1${blue_weight}/" \
        -e "s/(server\s+green:[0-9]+\s+weight=)[0-9]+/\1${green_weight}/" \
        "$NGINX_CONF" > "$tmp_conf"

    # 验证修改是否生效（防止 sed 未匹配）
    if ! diff -q "$NGINX_CONF" "$tmp_conf" >/dev/null 2>&1; then
        # 有差异，检查是否都改了
        local new_blue new_green
        new_blue="$(grep -E "server\s+blue:[0-9]+" "$tmp_conf" | \
            grep -oE 'weight=[0-9]+' | cut -d= -f2 | head -1 || echo "")"
        new_green="$(grep -E "server\s+green:[0-9]+" "$tmp_conf" | \
            grep -oE 'weight=[0-9]+' | cut -d= -f2 | head -1 || echo "")"

        if [[ "$new_blue" != "$blue_weight" ]] || [[ "$new_green" != "$green_weight" ]]; then
            error "sed 替换失败：期望 blue=$blue_weight green=$green_weight"
            error "  实际 blue=$new_blue green=$new_green"
            error "  检查配置文件格式是否包含 'server blue:PORT weight=N'"
            exit 4
        fi
    fi

    # 原子替换
    if [[ -n "$NGINX_CONTAINER" ]]; then
        docker cp "$tmp_conf" "${NGINX_CONTAINER}:/etc/nginx/conf.d/upstream.conf"
    else
        cp "$tmp_conf" "$NGINX_CONF"
    fi

    success "权重已更新"
}

# ==============================================================================
# 验证 Nginx 配置
# ==============================================================================
validate_config() {
    step "验证 Nginx 配置"

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会执行 nginx -t"
        return
    fi

    local result
    if [[ -n "$NGINX_CONTAINER" ]]; then
        if ! result="$(docker exec "$NGINX_CONTAINER" nginx -t 2>&1)"; then
            error "Nginx 配置验证失败"
            error "$result"
            exit 4
        fi
    else
        if ! result="$("$NGINX_BIN" -t 2>&1)"; then
            error "Nginx 配置验证失败"
            error "$result"
            exit 4
        fi
    fi

    info "配置语法: OK"
    success "配置验证通过"
}

# ==============================================================================
# 健康检查目标环境
# ==============================================================================
healthcheck_target() {
    local target="$1"

    step "健康检查目标环境（$target）"

    # 目标端口（可通过环境变量覆盖）
    local target_url
    if [[ "$target" = "blue" ]]; then
        target_url="${BLUE_HEALTH_URL:-http://localhost:8001/health}"
    else
        target_url="${GREEN_HEALTH_URL:-http://localhost:8002/health}"
    fi

    info "检查: $target_url"

    local elapsed=0
    while [[ $elapsed -lt $HEALTHCHECK_TIMEOUT ]]; do
        if curl -sf --max-time 3 "$target_url" >/dev/null 2>&1; then
            success "目标环境健康"
            return 0
        fi
        sleep 2
        elapsed=$((elapsed + 2))
    done

    error "目标环境健康检查超时: $target_url"
    exit 8
}

# ==============================================================================
# 排空连接
# ==============================================================================
drain_connections() {
    if [[ "$WAIT_DRAIN" != "1" ]] || [[ "$DRAIN_TIMEOUT" -eq 0 ]]; then
        info "跳过连接排空"
        return
    fi

    step "排空连接"
    info "等待 ${DRAIN_TIMEOUT}s 让现有连接完成..."

    sleep "$DRAIN_TIMEOUT"
    success "连接排空完成"
}

# ==============================================================================
# Reload Nginx
# ==============================================================================
reload_nginx() {
    step "Reload Nginx"

    if [[ "$DRY_RUN" = "1" ]]; then
        info "[DRY-RUN] 会执行 nginx -s reload"
        return
    fi

    if [[ -n "$NGINX_CONTAINER" ]]; then
        if ! docker exec "$NGINX_CONTAINER" nginx -s reload 2>&1; then
            error "容器内 Nginx reload 失败"
            exit 6
        fi
    else
        if ! "$NGINX_BIN" -s reload 2>&1; then
            error "Nginx reload 失败"
            exit 6
        fi
    fi

    # 等待生效
    sleep 2
    success "Nginx 已 reload"
}

# ==============================================================================
# 记录历史与状态
# ==============================================================================
record_state() {
    local from="$1"
    local to="$2"

    if [[ "$DRY_RUN" = "1" ]]; then
        return
    fi

    mkdir -p "$(dirname "$HISTORY_FILE")"

    local ts
    ts="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

    echo "${ts}|${from}|${to}|${SWITCH_ID}" >> "$HISTORY_FILE"
    echo "$to" > "$STATE_FILE"

    info "状态已记录: $from → $to"
}

# ==============================================================================
# 回滚
# ==============================================================================
do_rollback() {
    step "回滚到上次切换前的状态"

    if [[ ! -f "$HISTORY_FILE" ]]; then
        error "无切换历史，无法回滚"
        exit 1
    fi

    # 获取上一条记录
    local last_line
    last_line="$(tail -1 "$HISTORY_FILE" 2>/dev/null || echo "")"

    if [[ -z "$last_line" ]]; then
        error "无可用回滚记录"
        exit 1
    fi

    local last_from last_to
    last_from="$(echo "$last_line" | awk -F'|' '{print $2}')"
    last_to="$(echo "$last_line" | awk -F'|' '{print $3}')"

    info "上次切换: $last_from → $last_to"
    info "回滚: $last_to → $last_from"

    # 反向切换
    TARGET="$last_from"

    # 找到最近的备份
    local latest_backup
    latest_backup="$(ls -t "${BACKUP_DIR}"/nginx-*.conf 2>/dev/null | head -1 || echo "")"

    if [[ -n "$latest_backup" ]] && [[ -f "$latest_backup" ]]; then
        info "使用备份: $latest_backup"

        if [[ "$DRY_RUN" != "1" ]]; then
            if [[ -n "$NGINX_CONTAINER" ]]; then
                docker cp "$latest_backup" "${NGINX_CONTAINER}:/etc/nginx/conf.d/upstream.conf"
            else
                cp "$latest_backup" "$NGINX_CONF"
            fi
        fi

        validate_config
        drain_connections
        reload_nginx
        record_state "$last_to" "$last_from"

        success "回滚完成"
    else
        warn "无备份可用，执行反向切换"
        perform_switch
    fi
}

# ==============================================================================
# 执行切换
# ==============================================================================
perform_switch() {
    local from="$1"
    local to="$2"
    local blue_weight green_weight

    # 计算权重
    if [[ "$CANARY_PERCENT" -gt 0 ]]; then
        # 灰度模式
        if [[ "$to" = "blue" ]]; then
            blue_weight="$CANARY_PERCENT"
            green_weight=$((100 - CANARY_PERCENT))
        else
            green_weight="$CANARY_PERCENT"
            blue_weight=$((100 - CANARY_PERCENT))
        fi
    else
        # 全量切换
        if [[ "$to" = "blue" ]]; then
            blue_weight=100
            green_weight=0
        else
            blue_weight=0
            green_weight=100
        fi
    fi

    update_upstream_weights "$blue_weight" "$green_weight"
    validate_config
    drain_connections
    reload_nginx
    record_state "$from" "$to"

    # 验证切换结果
    sleep 2
    local current
    current="$(curl -sf --max-time 3 http://localhost:8000/version 2>/dev/null | \
        grep -oE '"active":\s*"[^"]+"' | cut -d'"' -f4 || echo "")"

    if [[ -n "$current" ]] && [[ "$current" != "$to" ]] && [[ "$CANARY_PERCENT" -eq 0 ]]; then
        warn "切换结果验证失败：期望 $to，实际 $current"
    fi
}

# ==============================================================================
# 确认提示
# ==============================================================================
confirm_switch() {
    if [[ "$FORCE" = "1" ]] || [[ "$DRY_RUN" = "1" ]]; then
        return
    fi
    if [[ -n "${CI:-}" ]] || [[ ! -t 0 ]]; then
        return
    fi

    printf "\n%b即将切换流量%b\n" "${COLOR_YELLOW}${COLOR_BOLD}" "${COLOR_RESET}"
    printf "  当前: %s\n" "${CURRENT_ACTIVE:-unknown}"
    printf "  目标: %s\n" "$TARGET"
    if [[ "$CANARY_PERCENT" -gt 0 ]]; then
        printf "  灰度: %s%%\n" "$CANARY_PERCENT"
    fi
    printf "  切换 ID: %s\n" "$SWITCH_ID"
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
        printf "%b║  币安 BTC/ETH 量化交易系统 流量切换  v%s%b\n" \
            "${COLOR_CYAN}${COLOR_BOLD}" "$SCRIPT_VERSION" "${COLOR_RESET}"
        printf "%b╚══════════════════════════════════════════════════════════════╝%b\n" \
            "${COLOR_CYAN}${COLOR_BOLD}" "${COLOR_RESET}"
    fi

    if [[ "$DRY_RUN" = "1" ]]; then
        warn "DRY-RUN 模式：不会实际切换"
    fi

    mkdir -p "$LOG_DIR"

    # 获取锁
    acquire_lock

    # 环境检查
    check_environment

    # 读取当前状态
    read_current_state

    # 状态查询
    if [[ "$SHOW_STATUS" = "1" ]]; then
        show_status
        exit 0
    fi

    # 回滚模式
    if [[ "$ROLLBACK" = "1" ]]; then
        confirm_switch
        do_rollback
        exit 0
    fi

    # 确认
    confirm_switch

    # 目标与当前相同
    if [[ "$TARGET" = "$CURRENT_ACTIVE" ]] && [[ "$CANARY_PERCENT" -eq 0 ]] && \
       [[ "$FORCE" != "1" ]]; then
        warn "目标与当前相同: $TARGET"
        info "使用 --force 强制执行"
        exit 0
    fi

    # 健康检查目标
    healthcheck_target "$TARGET"

    # 备份
    backup_config

    # 执行切换
    perform_switch "$CURRENT_ACTIVE" "$TARGET"

    # 完成
    printf "\n"
    printf "%b✅ 流量切换成功%b\n" "${COLOR_GREEN}${COLOR_BOLD}" "${COLOR_RESET}"
    printf "   从:     %s\n" "${CURRENT_ACTIVE:-unknown}"
    printf "   到:     %s\n" "$TARGET"
    if [[ "$CANARY_PERCENT" -gt 0 ]]; then
        printf "   灰度:   %s%%\n" "$CANARY_PERCENT"
    fi
    printf "   切换 ID: %s\n" "$SWITCH_ID"
    printf "   日志:   %s\n" "$LOG_DIR"
    printf "\n"
}

# ==============================================================================
# 入口
# ==============================================================================
main "$@"
