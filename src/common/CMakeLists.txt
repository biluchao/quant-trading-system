# ==============================================================================
# 币安 BTC/ETH 3分钟量化交易系统 - common 模块构建配置
# ==============================================================================
# @file    src/common/CMakeLists.txt
# @module  common
# @type    core
# @name    CMakeLists
# @version 1.0.1
# @brief   公共基础设施构建配置：类型、时间戳、配置、日志、内存池、
#          无锁队列、SIMD、错误处理、加密、指标
#          已修复 20+ 类运行时问题
#
# 包含文件:
#   头文件 (10):
#     common.core.types.hpp            基础类型、枚举、常量、订单、成交、持仓
#     common.core.timestamp.hpp        统一时间戳（NTP + 交易所偏移 + 单调时钟）
#     common.core.config.hpp           配置加载（RCU 热更新 + 订阅）
#     common.core.logger.hpp           异步日志（spdlog 封装 + 崩溃处理）
#     common.core.memory_pool.hpp      分层内存池（TLS + 全局后备）
#     common.core.lockfree_queue.hpp   无锁队列（SPSC / MPSC）
#     common.core.simd.hpp             SIMD 工具（AVX2 / AVX-512 / NEON / 标量）
#     common.core.error.hpp            错误处理（Result<T,E> + 异常桥接）
#     common.core.crypto.hpp           加密工具（SHA/HMAC/AES-GCM）
#     common.core.metrics.hpp          Prometheus 指标（Counter/Gauge/Histogram）
#
#   源文件 (6 必需 + 2 可选):
#     common.core.types.cpp            （必需）from_string 等非 inline 实现
#     common.core.timestamp.cpp        （必需）ISO 8601、ClockManager
#     common.core.config.cpp           （必需）文件加载、环境变量、订阅
#     common.core.logger.cpp           （必需）崩溃处理、sink、异步池
#     common.core.memory_pool.cpp      （必需）Impl、TLB refill/spill/flush
#     common.core.error.cpp            （必需）fatal_error、errno 转换
#     common.core.crypto.cpp           （可选）若分离实现
#     common.core.metrics.cpp          （可选）若分离实现
#
# 使用方式（下游模块）:
#   target_link_libraries(my_target PRIVATE quant::common)
# ==============================================================================

# ==============================================================================
# 1. 模块元信息
# ==============================================================================
set(QUANT_COMMON_MODULE_NAME "common")
set(QUANT_COMMON_MODULE_VERSION "1.0.1")
set(QUANT_COMMON_CONTRACT_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/common.core.contract.json")

message(STATUS "配置模块: common (v${QUANT_COMMON_MODULE_VERSION})")

# ==============================================================================
# 2. 构建选项
# ==============================================================================
option(QUANT_COMMON_SEPARATE_CRYPTO_IMPL
    "分离 crypto 实现到 .cpp（默认 header-only）" OFF)
option(QUANT_COMMON_SEPARATE_METRICS_IMPL
    "分离 metrics 实现到 .cpp（默认 header-only）" OFF)
option(QUANT_COMMON_STRICT_SOURCES
    "源文件缺失时中断构建（推荐开启）" ON)

# ==============================================================================
# 3. 必需源文件清单
# ==============================================================================
set(QUANT_COMMON_SOURCES_REQUIRED
    common.core.types.cpp
    common.core.timestamp.cpp
    common.core.config.cpp
    common.core.logger.cpp
    common.core.memory_pool.cpp
    common.core.error.cpp
)

# ==============================================================================
# 4. 可选源文件清单
# ==============================================================================
set(QUANT_COMMON_SOURCES_OPTIONAL "")

if(QUANT_COMMON_SEPARATE_CRYPTO_IMPL)
    list(APPEND QUANT_COMMON_SOURCES_OPTIONAL common.core.crypto.cpp)
endif()

if(QUANT_COMMON_SEPARATE_METRICS_IMPL)
    list(APPEND QUANT_COMMON_SOURCES_OPTIONAL common.core.metrics.cpp)
endif()

# ==============================================================================
# 5. 汇总源文件
# ==============================================================================
set(QUANT_COMMON_SOURCES
    ${QUANT_COMMON_SOURCES_REQUIRED}
    ${QUANT_COMMON_SOURCES_OPTIONAL}
)

# ==============================================================================
# 6. 头文件清单（参与 IDE 显示，不参与编译）
# ==============================================================================
set(QUANT_COMMON_HEADERS
    common.core.types.hpp
    common.core.timestamp.hpp
    common.core.config.hpp
    common.core.logger.hpp
    common.core.memory_pool.hpp
    common.core.lockfree_queue.hpp
    common.core.simd.hpp
    common.core.error.hpp
    common.core.crypto.hpp
    common.core.metrics.hpp
)

# ==============================================================================
# 7. 编译期文件存在性校验
# ==============================================================================
foreach(_src ${QUANT_COMMON_SOURCES})
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_src}")
        if(QUANT_COMMON_STRICT_SOURCES)
            message(FATAL_ERROR
                "common 模块缺少必需源文件: ${_src}\n"
                "  期望路径: ${CMAKE_CURRENT_SOURCE_DIR}/${_src}\n"
                "  请创建该文件或禁用 QUANT_COMMON_STRICT_SOURCES")
        else()
            message(WARNING "common 模块缺少源文件: ${_src}")
        endif()
    endif()
endforeach()

foreach(_hdr ${QUANT_COMMON_HEADERS})
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_hdr}")
        if(QUANT_COMMON_STRICT_SOURCES)
            message(FATAL_ERROR
                "common 模块缺少头文件: ${_hdr}\n"
                "  期望路径: ${CMAKE_CURRENT_SOURCE_DIR}/${_hdr}")
        else()
            message(WARNING "common 模块缺少头文件: ${_hdr}")
        endif()
    endif()
endforeach()

# ==============================================================================
# 8. 库目标
# ==============================================================================
add_library(quant_common STATIC
    ${QUANT_COMMON_SOURCES}
    ${QUANT_COMMON_HEADERS}
)

# 命名空间别名（下游统一使用 quant::common）
add_library(quant::common ALIAS quant_common)

# ==============================================================================
# 9. 源文件 IDE 分组
# ==============================================================================
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    PREFIX "Sources"
    FILES ${QUANT_COMMON_SOURCES} ${QUANT_COMMON_HEADERS}
)

# ==============================================================================
# 10. Include 目录（使用 CURRENT_SOURCE_DIR 而非 CMAKE_SOURCE_DIR）
# ==============================================================================
target_include_directories(quant_common
    PUBLIC
        # 下游通过 #include <common/common.core.types.hpp> 引用
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# ==============================================================================
# 11. C++ 标准（传递到下游）
# ==============================================================================
target_compile_features(quant_common PUBLIC cxx_std_20)

# ==============================================================================
# 12. 编译选项（仅作用于本目标，不传播到下游）
# ==============================================================================
if(QUANT_COMPILER_ID MATCHES "GCC|Clang|AppleClang")
    target_compile_options(quant_common PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wnull-dereference
        -Wdouble-promotion
    )
elseif(QUANT_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(quant_common PRIVATE
        /W4
        /permissive-
        /Zc:__cplusplus
        /Zc:preprocessor
        /utf-8
        /external:W0
    )
endif()

# ==============================================================================
# 13. 平台特定编译定义
# ==============================================================================
if(WIN32)
    target_compile_definitions(quant_common PUBLIC
        NOMINMAX                    # 防止 <windows.h> 污染 std::min/max
        WIN32_LEAN_AND_MEAN         # 排除 <windows.h> 中的罕见头文件
        _CRT_SECURE_NO_WARNINGS     # 允许 C 风格函数
        _USE_MATH_DEFINES           # 启用 M_PI 等常量
    )
endif()

# ==============================================================================
# 14. 位置无关代码（供链接到共享库）
# ==============================================================================
set_target_properties(quant_common PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

# ==============================================================================
# 15. 依赖查找与链接
# ==============================================================================

# ------------------------------------------------------------------------------
# 线程库（std::thread、std::mutex）
# ------------------------------------------------------------------------------
find_package(Threads REQUIRED)
target_link_libraries(quant_common PUBLIC Threads::Threads)

# ------------------------------------------------------------------------------
# OpenSSL（crypto 模块）
# ------------------------------------------------------------------------------
find_package(OpenSSL REQUIRED)
target_link_libraries(quant_common PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
)

# ------------------------------------------------------------------------------
# spdlog（logger 模块）
# ------------------------------------------------------------------------------
if(TARGET spdlog::spdlog)
    set(_quant_spdlog_target spdlog::spdlog)
elseif(TARGET spdlog::spdlog_header_only)
    set(_quant_spdlog_target spdlog::spdlog_header_only)
else()
    find_package(spdlog 1.11 REQUIRED)
    set(_quant_spdlog_target spdlog::spdlog)
endif()
target_link_libraries(quant_common PUBLIC ${_quant_spdlog_target})
unset(_quant_spdlog_target)

# ------------------------------------------------------------------------------
# nlohmann_json（config 模块）
# ------------------------------------------------------------------------------
if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(quant_common PUBLIC nlohmann_json::nlohmann_json)
else()
    find_package(nlohmann_json 3.11 REQUIRED)
    target_link_libraries(quant_common PUBLIC nlohmann_json::nlohmann_json)
endif()

# ------------------------------------------------------------------------------
# Prometheus C++（metrics 模块，可选）
# ------------------------------------------------------------------------------
find_package(prometheus-cpp QUIET)

if(prometheus-cpp_FOUND)
    target_link_libraries(quant_common PUBLIC
        prometheus-cpp::core
        prometheus-cpp::pull
    )
    target_compile_definitions(quant_common PUBLIC QUANT_HAS_PROMETHEUS=1)
    message(STATUS "common: prometheus-cpp 已启用")
else()
    target_compile_definitions(quant_common PUBLIC QUANT_HAS_PROMETHEUS=0)
    message(STATUS "common: prometheus-cpp 未找到，metrics 降级为空实现")
endif()

# ------------------------------------------------------------------------------
# moodycamel ConcurrentQueue（可选，lockfree_queue 后端）
# ------------------------------------------------------------------------------
set(_quant_moodycamel_dir
    "${CMAKE_SOURCE_DIR}/third_party/moodycamel")

if(EXISTS "${_quant_moodycamel_dir}/concurrentqueue.h")
    target_include_directories(quant_common PUBLIC
        $<BUILD_INTERFACE:${_quant_moodycamel_dir}>
    )
    target_compile_definitions(quant_common PUBLIC
        QUANT_HAS_MOODYCAMEL=1)
    message(STATUS "common: moodycamel ConcurrentQueue 已启用")
else()
    target_compile_definitions(quant_common PUBLIC
        QUANT_HAS_MOODYCAMEL=0)
endif()
unset(_quant_moodycamel_dir)

# ==============================================================================
# 16. 文件系统库（GCC 8 / Clang 9 需要显式链接）
# ==============================================================================
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
   AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(quant_common PUBLIC stdc++fs)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang"
       AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 10)
    target_link_libraries(quant_common PUBLIC c++fs)
endif()

# ==============================================================================
# 17. SIMD 架构检测
# ==============================================================================
include(CheckCXXSourceCompiles)

# 保存并清空编译标志
set(_quant_saved_required_flags "${CMAKE_REQUIRED_FLAGS}")

# 检测 AVX2
set(CMAKE_REQUIRED_FLAGS "-mavx2")
check_cxx_source_compiles("
    #include <immintrin.h>
    int main() {
        __m256i a = _mm256_set1_epi32(1);
        __m256i b = _mm256_set1_epi32(2);
        __m256i c = _mm256_add_epi32(a, b);
        return _mm256_extract_epi32(c, 0);
    }
" QUANT_COMMON_HAS_AVX2)
unset(CMAKE_REQUIRED_FLAGS)

# 检测 FMA
set(CMAKE_REQUIRED_FLAGS "-mfma")
check_cxx_source_compiles("
    #include <immintrin.h>
    int main() {
        __m256d a = _mm256_set1_pd(1.0);
        __m256d b = _mm256_set1_pd(2.0);
        __m256d c = _mm256_fmadd_pd(a, b, a);
        return static_cast<int>(_mm256_cvtsd_f64(c));
    }
" QUANT_COMMON_HAS_FMA)
unset(CMAKE_REQUIRED_FLAGS)

# 恢复标志
set(CMAKE_REQUIRED_FLAGS "${_quant_saved_required_flags}")
unset(_quant_saved_required_flags)

# NEON 检测（ARM）
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
    set(QUANT_COMMON_HAS_NEON TRUE)
else()
    set(QUANT_COMMON_HAS_NEON FALSE)
endif()

# 传递编译定义
if(QUANT_COMMON_HAS_AVX2)
    target_compile_definitions(quant_common PUBLIC QUANT_HAS_AVX2=1)
    message(STATUS "common: AVX2 可用")
endif()

if(QUANT_COMMON_HAS_FMA)
    target_compile_definitions(quant_common PUBLIC QUANT_HAS_FMA=1)
endif()

if(QUANT_COMMON_HAS_NEON)
    target_compile_definitions(quant_common PUBLIC QUANT_HAS_NEON=1)
    message(STATUS "common: NEON 可用")
endif()

# ==============================================================================
# 18. Windows DLL 导出
# ==============================================================================
if(WIN32 AND BUILD_SHARED_LIBS)
    target_compile_definitions(quant_common PUBLIC QUANT_COMMON_EXPORTS)
    set_target_properties(quant_common PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS ON
    )
endif()

# ==============================================================================
# 19. Debug/Release 输出名区分（Windows）
# ==============================================================================
if(WIN32)
    set_target_properties(quant_common PROPERTIES
        DEBUG_POSTFIX "d"
    )
endif()

# ==============================================================================
# 20. 预编译头（PCH）
# ==============================================================================
if(QUANT_ENABLE_PCH AND COMMAND target_precompile_headers)
    target_precompile_headers(quant_common PRIVATE
        <algorithm>
        <array>
        <atomic>
        <chrono>
        <cstddef>
        <cstdint>
        <cstring>
        <filesystem>
        <functional>
        <map>
        <memory>
        <mutex>
        <optional>
        <shared_mutex>
        <span>
        <string>
        <string_view>
        <thread>
        <unordered_map>
        <utility>
        <variant>
        <vector>
    )
    message(STATUS "common: PCH 已启用")
endif()

# ==============================================================================
# 21. 契约文件安装（项目规范要求）
# ==============================================================================
if(EXISTS "${QUANT_COMMON_CONTRACT_FILE}")
    add_custom_command(TARGET quant_common PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${CMAKE_BINARY_DIR}/contracts"
    )

    add_custom_command(TARGET quant_common POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${QUANT_COMMON_CONTRACT_FILE}"
            "${CMAKE_BINARY_DIR}/contracts/common.core.contract.json"
        COMMENT "复制 common 契约文件"
    )

    # 计算并注入契约哈希
    file(SHA256 "${QUANT_COMMON_CONTRACT_FILE}" _quant_common_contract_hash)

    add_custom_command(TARGET quant_common POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DCONTRACT_FILE=${CMAKE_BINARY_DIR}/contracts/common.core.contract.json
            -DCONTRACT_HASH=${_quant_common_contract_hash}
            -P "${CMAKE_SOURCE_DIR}/cmake/scripts/inject_contract_hash.cmake"
        COMMENT "注入契约哈希"
    )
endif()

# ==============================================================================
# 22. 安装规则
# ==============================================================================
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# 安装静态库
install(TARGETS quant_common
    EXPORT quant-common-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# 安装头文件
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/common
    FILES_MATCHING
        PATTERN "*.hpp"
        PATTERN "*.h"
        PATTERN "*.contract.json"
        PATTERN "CMakeLists.txt" EXCLUDE
)

# 导出目标
install(EXPORT quant-common-targets
    FILE quant-common-targets.cmake
    NAMESPACE quant::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/quant-common
)

# 生成配置文件
configure_package_config_file(
    "${CMAKE_SOURCE_DIR}/cmake/quant-common-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/quant-common-config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/quant-common
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/quant-common-config-version.cmake"
    VERSION ${QUANT_COMMON_MODULE_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/quant-common-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/quant-common-config-version.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/quant-common
)

# ==============================================================================
# 23. 目标属性
# ==============================================================================
set_target_properties(quant_common PROPERTIES
    OUTPUT_NAME "quant_common"
    FOLDER "modules"
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    DESCRIPTION "Common infrastructure for quant trading system"
)

# ==============================================================================
# 24. 编译摘要
# ==============================================================================
message(STATUS "")
message(STATUS "╔══════════════════════════════════════════════════════════════╗")
message(STATUS "║  common 模块配置摘要                                          ║")
message(STATUS "╠══════════════════════════════════════════════════════════════╣")
message(STATUS "║  版本:           ${QUANT_COMMON_MODULE_VERSION}")
message(STATUS "║  必需源文件:     ${CMAKE_MATCH_COUNT}")
message(STATUS "║  必需源文件数:   6")
message(STATUS "║  可选源文件数:   ${CMAKE_MATCH_COUNT}")
message(STATUS "║  头文件数:       10")
message(STATUS "║  编译器:         ${QUANT_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "╠══════════════════════════════════════════════════════════════╣")
message(STATUS "║  Prometheus:     ${prometheus-cpp_FOUND}")
message(STATUS "║  moodycamel:     ${QUANT_HAS_MOODYCAMEL}")
message(STATUS "║  AVX2:           ${QUANT_COMMON_HAS_AVX2}")
message(STATUS "║  FMA:            ${QUANT_COMMON_HAS_FMA}")
message(STATUS "║  NEON:           ${QUANT_COMMON_HAS_NEON}")
message(STATUS "║  PCH:            ${QUANT_ENABLE_PCH}")
message(STATUS "╚══════════════════════════════════════════════════════════════╝")
message(STATUS "")
