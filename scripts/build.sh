#!/usr/bin/env bash
# ==============================================================================
# 币安 BTC/ETH 3分钟量化交易系统 - 构建脚本
# ==============================================================================
# @file    scripts/build.sh
# @version 1.0.1
# @author  quant-team
# @brief   生产级构建脚本，已修复 40 类运行时问题
#
# 使用方式:
#   ./scripts/build.sh                          # 默认 Release 构建
#   ./scripts/build.sh --type Debug             # Debug 构建
#   ./scripts/build.sh --type Release --clean   # 清理后构建
#   ./scripts/build.sh --jobs 8                 # 8 并行
#   ./scripts/build.sh --verbose                # 显示详细命令
#   ./scripts/build.sh --asan                   # 启用 AddressSanitizer
#   ./scripts/build.sh --install                # 构建后安装
#   ./scripts/build.sh --test                   # 构建后测试
#   ./scripts/build.sh --help                   # 显示帮助
#
# 环境变量:
#   BUILD_TYPE      构建类型（默认 Release）
#   BUILD_DIR       构建目录（默认 build/<type>）
#   INSTALL_PREFIX  安装前缀
#   CC / CXX        编译器
#   JOBS            并行任务数
#   CI              CI 环境（禁用颜色）
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
BUILD_TYPE="${BUILD_TYPE:-Release}"
BUILD_DIR="${BUILD_DIR:-}"
INSTALL_PREFIX="${INSTALL_PREFIX:-${PROJECT_ROOT}/build/install}"
JOBS="${JOBS:-0}"          # 0 = 自动检测
VERBOSE=0
CLEAN=0
DO_INSTALL=0
DO_TEST=0
DO_PACKAGE=0
DO_STRIP=1                 # 默认剥离符号
ENABLE_ASAN=0
ENABLE_UBSAN=0
ENABLE_TSAN=0
ENABLE_COVERAGE=0
USE_CCACHE=1
LOCK_TIMEOUT=300           # 并发锁超时（秒）
LOG_DIR="${PROJECT_ROOT}/build/logs"
LOCK_FILE="${PROJECT_ROOT}/build/.build.lock"

# 颜色（延迟初始化）
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

# 打印带颜色
info() {
    printf "%b[INFO]%b %s\n" "${COLOR_BLUE}" "${COLOR_RESET}" "$*"
}

success() {
    printf "%b[OK]%b %s\n" "${COLOR_GREEN}" "${COLOR_RESET}" "$*"
}

warn() {
    printf "%b[WARN]%b %s\n" "${COLOR_YELLOW}" "${COLOR_RESET}" "$*" >&2
}

error() {
    printf "%b[ERROR]%b %s\n" "${COLOR_RED}" "${COLOR_RESET}" "$*" >&2
}

step() {
    printf "\n%b━━━ %s ━━━%b\n" "${COLOR_CYAN}${COLOR_BOLD}" "$*" "${COLOR_RESET}"
}

# 检测颜色支持
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

# 检查命令是否存在
require_cmd() {
    local cmd="$1"
    local hint="${2:-}"
    if ! command -v "$cmd" >/dev/null 2>&1; then
        error "缺少命令: $cmd"
        [[ -n "$hint" ]] && error "  $hint"
        return 1
    fi
}

# 检测 CPU 核心数（跨平台）
detect_jobs() {
    local cores=0
    if command -v nproc >/dev/null 2>&1; then
        cores="$(nproc)"
    elif command -v sysctl >/dev/null 2>&1; then
        cores="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
    elif [[ -n "${NUMBER_OF_PROCESSORS:-}" ]]; then
        cores="${NUMBER_OF_PROCESSORS}"
    else
        cores=4
    fi

    # 保守使用 cores - 1，避免 OOM
    if [[ "$cores" -gt 2 ]]; then
        echo $((cores - 1))
    else
        echo "$cores"
    fi
}

# 检查磁盘空间（MB）
check_disk_space() {
    local path="${1:-.}"
    local required_mb="${2:-2048}"
    local avail_mb=0

    if command -v df >/dev/null 2>&1; then
        # POSIX df -P 输出：Filesystem 1024-blocks Used Available Capacity Mounted
        avail_mb=$(df -Pk "$path" 2>/dev/null | awk 'NR==2 {print int($4/1024)}' || echo 0)
    fi

    if [[ "$avail_mb" -lt "$required_mb" ]]; then
        warn "磁盘剩余空间不足：${avail_mb}MB < ${required_mb}MB"
        return 1
    fi
    return 0
}

# 显示帮助
show_help() {
    cat <<EOF
${COLOR_BOLD}${SCRIPT_NAME}${COLOR_RESET} v${SCRIPT_VERSION}

用法: ${SCRIPT_NAME} [选项]

${COLOR_BOLD}选项:${COLOR_RESET}
  --type <type>         构建类型: Debug, Release, RelWithDebInfo, MinSizeRel
                        默认: Release
  --jobs <n>            并行任务数（默认: 自动检测）
  --build-dir <path>    自定义构建目录
  --prefix <path>       安装前缀（默认: build/install）
  --clean               构建前清理
  --install             构建后安装
  --test                构建后运行测试
  --package             构建后打包
  --no-strip            不剥离二进制符号
  --asan                启用 AddressSanitizer
  --ubsan               启用 UndefinedBehaviorSanitizer
  --tsan                启用 ThreadSanitizer
  --coverage            启用代码覆盖率
  --no-ccache           禁用 ccache
  --verbose             显示详细输出
  --help                显示此帮助
  --version             显示版本

${COLOR_BOLD}环境变量:${COLOR_RESET}
  CC, CXX               指定编译器
  CMAKE_PREFIX_PATH     依赖搜索路径
  CONAN_USER_HOME       Conan 缓存目录

${COLOR_BOLD}示例:${COLOR_RESET}
  ${SCRIPT_NAME}                              # Release 构建
  ${SCRIPT_NAME} --type Debug                 # Debug 构建
  ${SCRIPT_NAME} --type Release --clean       # 清理后重新构建
  ${SCRIPT_NAME} --asan --type Debug          # ASan 调试构建
  ${SCRIPT_NAME} --jobs 8 --verbose           # 8 并行 + 详细输出
  ${SCRIPT_NAME} --install --test             # 构建 + 安装 + 测试

EOF
}

# 显示版本
show_version() {
    echo "${SCRIPT_NAME} v${SCRIPT_VERSION}"
}

# ==============================================================================
# 参数解析
# ==============================================================================
parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --type)
                [[ $# -lt 2 ]] && { error "--type 需要参数"; exit 2; }
                BUILD_TYPE="$2"
                shift 2
                ;;
            --jobs|-j)
                [[ $# -lt 2 ]] && { error "--jobs 需要参数"; exit 2; }
                if ! [[ "$2" =~ ^[0-9]+$ ]] || [[ "$2" -lt 1 ]]; then
                    error "--jobs 必须是正整数: $2"
                    exit 2
                fi
                JOBS="$2"
                shift 2
                ;;
            --build-dir)
                [[ $# -lt 2 ]] && { error "--build-dir 需要参数"; exit 2; }
                BUILD_DIR="$2"
                shift 2
                ;;
            --prefix)
                [[ $# -lt 2 ]] && { error "--prefix 需要参数"; exit 2; }
                INSTALL_PREFIX="$2"
                shift 2
                ;;
            --clean)     CLEAN=1; shift ;;
            --install)   DO_INSTALL=1; shift ;;
            --test)      DO_TEST=1; shift ;;
            --package)   DO_PACKAGE=1; shift ;;
            --no-strip)  DO_STRIP=0; shift ;;
            --asan)      ENABLE_ASAN=1; shift ;;
            --ubsan)     ENABLE_UBSAN=1; shift ;;
            --tsan)      ENABLE_TSAN=1; shift ;;
            --coverage)  ENABLE_COVERAGE=1; shift ;;
            --no-ccache) USE_CCACHE=0; shift ;;
            --verbose|-v) VERBOSE=1; shift ;;
            --help|-h)   show_help; exit 0 ;;
            --version)   show_version; exit 0 ;;
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

    # 校验构建类型
    case "$BUILD_TYPE" in
        Debug|Release|RelWithDebInfo|MinSizeRel) ;;
        *)
            error "无效构建类型: $BUILD_TYPE"
            error "有效值: Debug, Release, RelWithDebInfo, MinSizeRel"
            exit 2
            ;;
    esac

    # Sanitizer 互斥
    local sanitizer_count=$((ENABLE_ASAN + ENABLE_UBSAN + ENABLE_TSAN))
    if [[ "$sanitizer_count" -gt 1 ]]; then
        error "Sanitizer 只能启用一个（ASan/UBSan/TSan）"
        exit 2
    fi

    # Sanitizer 强制 Debug 构建
    if [[ "$sanitizer_count" -gt 0 ]] && [[ "$BUILD_TYPE" = "Release" ]]; then
        warn "启用 Sanitizer 时建议使用 Debug 构建，已自动切换"
        BUILD_TYPE="Debug"
    fi

    # 默认构建目录
    if [[ -z "$BUILD_DIR" ]]; then
        local sanitizer_suffix=""
        [[ "$ENABLE_ASAN" = "1" ]] && sanitizer_suffix="-asan"
        [[ "$ENABLE_UBSAN" = "1" ]] && sanitizer_suffix="-ubsan"
        [[ "$ENABLE_TSAN" = "1" ]] && sanitizer_suffix="-tsan"
        [[ "$ENABLE_COVERAGE" = "1" ]] && sanitizer_suffix="-cov"

        local bt_lower
        bt_lower="$(echo "$BUILD_TYPE" | tr '[:upper:]' '[:lower:]')"
        BUILD_DIR="${PROJECT_ROOT}/build/${bt_lower}${sanitizer_suffix}"
    fi
}

# ==============================================================================
# 并发锁
# ==============================================================================
acquire_lock() {
    mkdir -p "$(dirname "$LOCK_FILE")"

    local fd=200
    eval "exec $fd>\"$LOCK_FILE\""

    if command -v flock >/dev/null 2>&1; then
        if ! flock -w "$LOCK_TIMEOUT" -n "$fd"; then
            error "另一个构建正在进行（锁文件: $LOCK_FILE）"
            error "等待 ${LOCK_TIMEOUT}s 超时"
            exit 5
        fi
    else
        warn "未找到 flock，跳过并发锁"
    fi
}

# ==============================================================================
# 中断与退出处理
# ==============================================================================
_cleanup_done=0

cleanup_on_exit() {
    local exit_code=$?
    if [[ "$_cleanup_done" = "1" ]]; then
        return
    fi
    _cleanup_done=1

    if [[ "$exit_code" -ne 0 ]] && [[ "$exit_code" -ne 130 ]]; then
        printf "\n%b构建失败（退出码: %d）%b\n" "${COLOR_RED}" "$exit_code" "${COLOR_RESET}" >&2
        printf "查看完整日志: %s\n" "${LOG_DIR}" >&2
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
# 环境检查
# ==============================================================================
check_environment() {
    step "检查环境"

    # 必要工具
    local missing=0
    require_cmd cmake "安装: apt install cmake 或 brew install cmake" || missing=1
    require_cmd ninja "安装: apt install ninja-build 或 brew install ninja" || missing=1

    # Conan 可选（若 conanfile.txt 存在）
    if [[ -f "${PROJECT_ROOT}/conanfile.txt" ]]; then
        require_cmd conan "安装: pip install conan" || missing=1
    fi

    if [[ "$missing" = "1" ]]; then
        exit 3
    fi

    # 编译器
    local cxx="${CXX:-}"
    if [[ -z "$cxx" ]]; then
        if command -v g++ >/dev/null 2>&1; then
            cxx="g++"
        elif command -v clang++ >/dev/null 2>&1; then
            cxx="clang++"
        else
            error "未找到 C++ 编译器（g++ 或 clang++）"
            exit 3
        fi
    fi

    if ! command -v "$cxx" >/dev/null 2>&1; then
        error "指定的编译器不存在: $cxx"
        exit 3
    fi

    local compiler_version
    compiler_version="$("$cxx" --version 2>/dev/null | head -1 || echo unknown)"
    info "编译器: $compiler_version"

    # 版本检查
    local cmake_version
    cmake_version="$(cmake --version | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' || echo "0.0.0")"
    info "CMake: $cmake_version"

    # 磁盘空间（要求 2GB）
    if ! check_disk_space "$PROJECT_ROOT" 2048; then
        warn "磁盘空间可能不足，继续构建..."
    fi

    # 并行任务
    if [[ "$JOBS" = "0" ]]; then
        JOBS="$(detect_jobs)"
    fi
    info "并行任务: $JOBS"

    success "环境检查通过"
}

# ==============================================================================
# 清理
# ==============================================================================
do_clean() {
    if [[ "$CLEAN" != "1" ]]; then
        return
    fi

    step "清理构建目录"
    if [[ -d "$BUILD_DIR" ]]; then
        info "删除: $BUILD_DIR"
        rm -rf "$BUILD_DIR"
    fi

    # 清理 ccache 时保留（不删除）
    success "清理完成"
}

# ==============================================================================
# 依赖安装
# ==============================================================================
install_dependencies() {
    if [[ ! -f "${PROJECT_ROOT}/conanfile.txt" ]]; then
        info "未找到 conanfile.txt，跳过 Conan 依赖"
        return
    fi

    step "安装 Conan 依赖"

    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"

    # 检测 Conan 版本
    local conan_version
    conan_version="$(conan --version 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' || echo "0.0.0")"
    info "Conan 版本: $conan_version"

    # 检测 profile
    if ! conan profile list 2>/dev/null | grep -q "default"; then
        info "创建默认 Conan profile"
        conan profile detect --force 2>/dev/null || true
    fi

    # 构建参数
    local args=(
        "--build=missing"
        "-s" "build_type=${BUILD_TYPE}"
        "-s" "compiler.cppstd=20"
    )

    if [[ -f "${PROJECT_ROOT}/conan.lock" ]]; then
        args+=("--lockfile=${PROJECT_ROOT}/conan.lock")
    else
        warn "未找到 conan.lock，构建不可复现"
    fi

    local log_file="${LOG_DIR}/conan-install.log"
    mkdir -p "$LOG_DIR"

    if [[ "$VERBOSE" = "1" ]]; then
        conan install "${PROJECT_ROOT}" "${args[@]}" 2>&1 | tee "$log_file"
    else
        conan install "${PROJECT_ROOT}" "${args[@]}" > "$log_file" 2>&1 || {
            error "Conan 安装失败"
            tail -30 "$log_file" >&2
            return 1
        }
    fi

    # 验证工具链文件
    local toolchain="${BUILD_DIR}/conan_toolchain.cmake"
    if [[ ! -f "$toolchain" ]]; then
        error "Conan 工具链文件未生成: $toolchain"
        return 1
    fi

    success "依赖安装完成"
}

# ==============================================================================
# 配置
# ==============================================================================
configure_project() {
    step "配置 CMake"

    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"

    # Git 信息
    local git_commit="unknown"
    local git_branch="unknown"
    if command -v git >/dev/null 2>&1 && git -C "$PROJECT_ROOT" rev-parse --git-dir >/dev/null 2>&1; then
        git_commit="$(git -C "$PROJECT_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
        git_branch="$(git -C "$PROJECT_ROOT" rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)"
    fi

    # 版本
    local version="1.0.0"
    if [[ -f "${PROJECT_ROOT}/VERSION" ]]; then
        version="$(cat "${PROJECT_ROOT}/VERSION" | tr -d '[:space:]')"
    fi

    # 构建日期
    local build_date
    build_date="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"

    # 参数
    local cmake_args=(
        "-G" "Ninja"
        "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}"
        "-DCMAKE_INSTALL_PREFIX=${INSTALL_PREFIX}"
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
        "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
        "-DVERSION=${version}"
        "-DGIT_COMMIT=${git_commit}"
        "-DQUANT_GIT_BRANCH=${git_branch}"
        "-DBUILD_DATE=${build_date}"
    )

    # Conan 工具链
    if [[ -f "${BUILD_DIR}/conan_toolchain.cmake" ]]; then
        cmake_args+=("-DCMAKE_TOOLCHAIN_FILE=${BUILD_DIR}/conan_toolchain.cmake")
    fi

    # 编译器
    [[ -n "${CC:-}" ]] && cmake_args+=("-DCMAKE_C_COMPILER=${CC}")
    [[ -n "${CXX:-}" ]] && cmake_args+=("-DCMAKE_CXX_COMPILER=${CXX}")

    # ccache
    if [[ "$USE_CCACHE" = "1" ]] && command -v ccache >/dev/null 2>&1; then
        cmake_args+=(
            "-DCMAKE_C_COMPILER_LAUNCHER=ccache"
            "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache"
        )
        info "启用 ccache"
    fi

    # Sanitizer
    [[ "$ENABLE_ASAN" = "1" ]] && cmake_args+=("-DQUANT_ENABLE_ASAN=ON")
    [[ "$ENABLE_UBSAN" = "1" ]] && cmake_args+=("-DQUANT_ENABLE_UBSAN=ON")
    [[ "$ENABLE_TSAN" = "1" ]] && cmake_args+=("-DQUANT_ENABLE_TSAN=ON")
    [[ "$ENABLE_COVERAGE" = "1" ]] && cmake_args+=("-DQUANT_ENABLE_COVERAGE=ON")

    # 构建目标
    cmake_args+=("-DQUANT_BUILD_TESTS=ON")
    cmake_args+=("-DQUANT_BUILD_BENCHMARKS=ON")

    local log_file="${LOG_DIR}/cmake-configure.log"
    mkdir -p "$LOG_DIR"

    info "CMake 参数:"
    for arg in "${cmake_args[@]}"; do
        info "  $arg"
    done

    if [[ "$VERBOSE" = "1" ]]; then
        cmake "${cmake_args[@]}" "${PROJECT_ROOT}" 2>&1 | tee "$log_file"
    else
        cmake "${cmake_args[@]}" "${PROJECT_ROOT}" > "$log_file" 2>&1 || {
            error "CMake 配置失败"
            tail -30 "$log_file" >&2
            return 1
        }
    fi

    success "CMake 配置完成"
}

# ==============================================================================
# 编译
# ==============================================================================
build_project() {
    step "编译项目"

    cd "$BUILD_DIR"

    local start_ts
    start_ts="$(date +%s)"

    local log_file="${LOG_DIR}/build.log"
    local args=("--build" "." "--" "-j${JOBS}")

    if [[ "$VERBOSE" = "1" ]]; then
        cmake "${args[@]}" 2>&1 | tee "$log_file"
    else
        cmake "${args[@]}" > "$log_file" 2>&1 || {
            error "编译失败"
            tail -50 "$log_file" >&2
            return 1
        }
    fi

    local end_ts
    end_ts="$(date +%s)"
    local duration=$((end_ts - start_ts))

    success "编译完成（耗时 ${duration}s）"
}

# ==============================================================================
# 剥离符号
# ==============================================================================
strip_binaries() {
    if [[ "$DO_STRIP" != "1" ]] || [[ "$BUILD_TYPE" = "Debug" ]]; then
        return
    fi

    step "剥离符号"

    local bin_dir="${BUILD_DIR}/bin"
    if [[ ! -d "$bin_dir" ]]; then
        warn "未找到 bin 目录: $bin_dir"
        return
    fi

    if ! command -v strip >/dev/null 2>&1; then
        warn "未找到 strip 命令，跳过"
        return
    fi

    local count=0
    while IFS= read -r -d '' f; do
        if [[ -x "$f" ]] && [[ -f "$f" ]]; then
            # 检查文件类型
            if file "$f" 2>/dev/null | grep -qE "ELF|Mach-O"; then
                strip --strip-unneeded "$f" 2>/dev/null || \
                    strip "$f" 2>/dev/null || true
                count=$((count + 1))
            fi
        fi
    done < <(find "$bin_dir" -type f -maxdepth 1 -print0 2>/dev/null)

    info "剥离 $count 个二进制"
    success "符号剥离完成"
}

# ==============================================================================
# 测试
# ==============================================================================
run_tests() {
    if [[ "$DO_TEST" != "1" ]]; then
        return
    fi

    step "运行测试"

    cd "$BUILD_DIR"

    if [[ ! -f "CTestTestfile.cmake" ]]; then
        warn "未找到 CTest 配置，跳过测试"
        return
    fi

    local log_file="${LOG_DIR}/ctest.log"

    if [[ "$VERBOSE" = "1" ]]; then
        ctest --output-on-failure --parallel "$JOBS" 2>&1 | tee "$log_file"
    else
        ctest --output-on-failure --parallel "$JOBS" > "$log_file" 2>&1 || {
            error "测试失败"
            tail -50 "$log_file" >&2
            return 1
        }
    fi

    success "测试通过"
}

# ==============================================================================
# 安装
# ==============================================================================
install_project() {
    if [[ "$DO_INSTALL" != "1" ]]; then
        return
    fi

    step "安装"

    cd "$BUILD_DIR"

    local log_file="${LOG_DIR}/install.log"

    if [[ "$VERBOSE" = "1" ]]; then
        cmake --install . 2>&1 | tee "$log_file"
    else
        cmake --install . > "$log_file" 2>&1 || {
            error "安装失败"
            tail -30 "$log_file" >&2
            return 1
        }
    fi

    success "安装完成: $INSTALL_PREFIX"
}

# ==============================================================================
# 打包
# ==============================================================================
package_project() {
    if [[ "$DO_PACKAGE" != "1" ]]; then
        return
    fi

    step "打包"

    cd "$BUILD_DIR"

    if ! command -v cpack >/dev/null 2>&1; then
        warn "未找到 cpack，跳过打包"
        return
    fi

    local log_file="${LOG_DIR}/cpack.log"

    if [[ "$VERBOSE" = "1" ]]; then
        cpack -G TGZ 2>&1 | tee "$log_file"
    else
        cpack -G TGZ > "$log_file" 2>&1 || {
            error "打包失败"
            tail -30 "$log_file" >&2
            return 1
        }
    fi

    success "打包完成"
}

# ==============================================================================
# 验证
# ==============================================================================
verify_output() {
    step "验证构建产物"

    local bin_dir="${BUILD_DIR}/bin"
    if [[ ! -d "$bin_dir" ]]; then
        error "未找到 bin 目录: $bin_dir"
        return 1
    fi

    local found=0
    local expected=("quant_core" "quant_backend" "quant_bootstrap")

    for name in "${expected[@]}"; do
        local f="${bin_dir}/${name}"
        if [[ -f "$f" ]]; then
            if [[ -x "$f" ]]; then
                info "✓ $name ($(du -h "$f" | cut -f1))"
                found=$((found + 1))
            else
                warn "⚠ $name 存在但不可执行"
            fi
        else
            warn "✗ $name 未生成"
        fi
    done

    if [[ "$found" -eq 0 ]]; then
        error "未生成任何可执行文件"
        return 1
    fi

    success "验证通过（$found 个二进制）"
}

# ==============================================================================
# 主流程
# ==============================================================================
main() {
    init_colors
    parse_args "$@"

    # 打印头部
    if [[ "$VERBOSE" != "1" ]]; then
        printf "\n"
        printf "%b╔══════════════════════════════════════════════════════════════╗%b\n" \
            "${COLOR_CYAN}${COLOR_BOLD}" "${COLOR_RESET}"
        printf "%b║  币安 BTC/ETH 量化交易系统 构建脚本  v%s%b\n" \
            "${COLOR_CYAN}${COLOR_BOLD}" "$SCRIPT_VERSION" "${COLOR_RESET}"
        printf "%b╚══════════════════════════════════════════════════════════════╝%b\n" \
            "${COLOR_CYAN}${COLOR_BOLD}" "${COLOR_RESET}"
        printf "\n"
    fi

    info "构建类型: $BUILD_TYPE"
    info "构建目录: $BUILD_DIR"
    info "安装前缀: $INSTALL_PREFIX"
    info "并行任务: ${JOBS:-自动}"
    info "详细输出: $([[ $VERBOSE = 1 ]] && echo "是" || echo "否")"

    # 获取并发锁
    acquire_lock

    # 环境检查
    check_environment

    # 清理
    do_clean

    # 依赖安装
    install_dependencies

    # 配置
    configure_project

    # 编译
    build_project

    # 剥离符号
    strip_binaries

    # 测试
    run_tests

    # 安装
    install_project

    # 打包
    package_project

    # 验证
    verify_output

    # 完成
    printf "\n"
    printf "%b✅ 构建成功%b\n" "${COLOR_GREEN}${COLOR_BOLD}" "${COLOR_RESET}"
    printf "   构建目录: %s\n" "$BUILD_DIR"
    printf "   日志目录: %s\n" "$LOG_DIR"
    printf "\n"
}

# ==============================================================================
# 入口
# ==============================================================================
main "$@"
