# ==============================================================================
# 币安 BTC/ETH 3分钟量化交易系统 - strategy 模块构建配置
# ==============================================================================
# @file    src/strategy/CMakeLists.txt
# @module  strategy
# @type    core
# @name    CMakeLists
# @version 1.0.1
# @brief   策略引擎与风控构建配置：状态机、评分、持仓、风控、追踪、监控
#          已修复 30 类运行时问题
#
# 包含文件:
#   头文件 (16):
#     strategy.core.engine.hpp              状态机 S0~S6
#     strategy.core.scoring.hpp             多因子正交化评分
#     strategy.core.position_manager.hpp    持仓管理（止损/加仓/止盈）
#     strategy.core.risk_check.hpp          盈亏比第一原则校验
#     strategy.core.config_mapper.hpp       参数映射与热更新
#     strategy.core.state_store.hpp         状态持久化
#     strategy.core.cooldown.hpp            冷却期管理
#     strategy.core.mtf_checker.hpp         多时间框架一致性
#     strategy.core.trend_filter.hpp        EMA 双均线趋势过滤
#     strategy.core.escape_detector.hpp     逃顶/逃底检测
#     strategy.core.reverse_scorer.hpp      反向单独立评分
#     strategy.core.strategies.hpp          5 种策略统一接口
#     strategy.core.portfolio_risk.hpp      组合风险（相关性加权）
#     strategy.core.signal_tracer.hpp       信号溯源
#     strategy.core.risk_monitor.hpp        账户级风险监控
#     strategy.core.correlation_monitor.hpp 相关性监控
#
#   源文件 (16):
#     与头文件一一对应
#
#   契约文件 (1):
#     strategy.core.contract.json
#
# 依赖:
#   - common (必需)
#   - indicator (必需)
#   - ai (可选，缺失时降级)
#   - spdlog, nlohmann_json, Threads (必需)
#
# 使用方式（下游模块）:
#   target_link_libraries(my_target PRIVATE quant::strategy)
# ==============================================================================

# ==============================================================================
# 1. 模块元信息
# ==============================================================================
set(QUANT_STRATEGY_MODULE_NAME "strategy")
set(QUANT_STRATEGY_MODULE_VERSION "1.0.1")
set(QUANT_STRATEGY_CONTRACT_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/strategy.core.contract.json")

message(STATUS "配置模块: strategy (v${QUANT_STRATEGY_MODULE_VERSION})")

# ==============================================================================
# 2. 构建选项
# ==============================================================================
option(QUANT_STRATEGY_ENABLE_AI
    "启用 AI 模块集成（缺失时降级）" ON)
option(QUANT_STRATEGY_STRICT_SOURCES
    "源文件缺失时中断构建（推荐开启）" ON)
option(QUANT_STRATEGY_ENABLE_DEBUG_POSTFIX
    "Windows 上 Debug 版本加 d 后缀" ON)
option(QUANT_STRATEGY_ENABLE_CONTRACT_HASH
    "注入契约文件 SHA-256 哈希" ON)

# ==============================================================================
# 3. 必需源文件清单（16 个）
# ==============================================================================
set(QUANT_STRATEGY_SOURCES_REQUIRED
    strategy.core.engine.cpp
    strategy.core.scoring.cpp
    strategy.core.position_manager.cpp
    strategy.core.risk_check.cpp
    strategy.core.config_mapper.cpp
    strategy.core.state_store.cpp
    strategy.core.cooldown.cpp
    strategy.core.mtf_checker.cpp
    strategy.core.trend_filter.cpp
    strategy.core.escape_detector.cpp
    strategy.core.reverse_scorer.cpp
    strategy.core.strategies.cpp
    strategy.core.portfolio_risk.cpp
    strategy.core.signal_tracer.cpp
    strategy.core.risk_monitor.cpp
    strategy.core.correlation_monitor.cpp
)

set(QUANT_STRATEGY_HEADERS
    strategy.core.engine.hpp
    strategy.core.scoring.hpp
    strategy.core.position_manager.hpp
    strategy.core.risk_check.hpp
    strategy.core.config_mapper.hpp
    strategy.core.state_store.hpp
    strategy.core.cooldown.hpp
    strategy.core.mtf_checker.hpp
    strategy.core.trend_filter.hpp
    strategy.core.escape_detector.hpp
    strategy.core.reverse_scorer.hpp
    strategy.core.strategies.hpp
    strategy.core.portfolio_risk.hpp
    strategy.core.signal_tracer.hpp
    strategy.core.risk_monitor.hpp
    strategy.core.correlation_monitor.hpp
)

# ==============================================================================
# 4. 编译期文件存在性校验
# ==============================================================================
foreach(_src ${QUANT_STRATEGY_SOURCES_REQUIRED})
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_src}")
        if(QUANT_STRATEGY_STRICT_SOURCES)
            message(FATAL_ERROR
                "strategy 模块缺少必需源文件: ${_src}\n"
                "  期望路径: ${CMAKE_CURRENT_SOURCE_DIR}/${_src}\n"
                "  请创建该文件或禁用 QUANT_STRATEGY_STRICT_SOURCES")
        else()
            message(WARNING "strategy 模块缺少源文件: ${_src}")
        endif()
    endif()
endforeach()

foreach(_hdr ${QUANT_STRATEGY_HEADERS})
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_hdr}")
        if(QUANT_STRATEGY_STRICT_SOURCES)
            message(FATAL_ERROR
                "strategy 模块缺少头文件: ${_hdr}\n"
                "  期望路径: ${CMAKE_CURRENT_SOURCE_DIR}/${_hdr}")
        else()
            message(WARNING "strategy 模块缺少头文件: ${_hdr}")
        endif()
    endif()
endforeach()

# ==============================================================================
# 5. 源文件数量校验（与契约声明一致性）
# ==============================================================================
list(LENGTH QUANT_STRATEGY_SOURCES_REQUIRED _src_count)
list(LENGTH QUANT_STRATEGY_HEADERS _hdr_count)

if(NOT _src_count EQUAL 16)
    message(WARNING
        "strategy 源文件数量 ${_src_count} 与预期 16 不一致，"
        "请同步更新 strategy.core.contract.json")
endif()

if(NOT _hdr_count EQUAL 16)
    message(WARNING
        "strategy 头文件数量 ${_hdr_count} 与预期 16 不一致，"
        "请同步更新 strategy.core.contract.json")
endif()

unset(_src_count)
unset(_hdr_count)

# ==============================================================================
# 6. 库目标
# ==============================================================================
add_library(quant_strategy STATIC
    ${QUANT_STRATEGY_SOURCES_REQUIRED}
    ${QUANT_STRATEGY_HEADERS}
)

# 命名空间别名
add_library(quant::strategy ALIAS quant_strategy)

# ==============================================================================
# 7. 源文件 IDE 分组
# ==============================================================================
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    PREFIX "Sources"
    FILES ${QUANT_STRATEGY_SOURCES_REQUIRED} ${QUANT_STRATEGY_HEADERS}
)

# ==============================================================================
# 8. Include 目录
# ==============================================================================
target_include_directories(quant_strategy
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# ==============================================================================
# 9. C++ 标准（传递到下游）
# ==============================================================================
target_compile_features(quant_strategy PUBLIC cxx_std_20)

# ==============================================================================
# 10. 编译选项（仅本目标，不传播）
# ==============================================================================
if(QUANT_COMPILER_ID MATCHES "GCC|Clang|AppleClang")
    target_compile_options(quant_strategy PRIVATE
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
    target_compile_options(quant_strategy PRIVATE
        /W4
        /permissive-
        /Zc:__cplusplus
        /Zc:preprocessor
        /utf-8
        /external:W0
    )
endif()

# ==============================================================================
# 11. 平台特定编译定义
# ==============================================================================
if(WIN32)
    target_compile_definitions(quant_strategy PUBLIC
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
        _USE_MATH_DEFINES
    )
endif()

# ==============================================================================
# 12. 位置无关代码
# ==============================================================================
set_target_properties(quant_strategy PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

# ==============================================================================
# 13. 依赖查找与链接
# ==============================================================================

# ------------------------------------------------------------------------------
# 线程库
# ------------------------------------------------------------------------------
find_package(Threads REQUIRED)
target_link_libraries(quant_strategy PUBLIC Threads::Threads)

# ------------------------------------------------------------------------------
# spdlog（日志）
# ------------------------------------------------------------------------------
if(TARGET spdlog::spdlog)
    set(_quant_strategy_spdlog_target spdlog::spdlog)
elseif(TARGET spdlog::spdlog_header_only)
    set(_quant_strategy_spdlog_target spdlog::spdlog_header_only)
else()
    find_package(spdlog 1.11 REQUIRED)
    set(_quant_strategy_spdlog_target spdlog::spdlog)
endif()
target_link_libraries(quant_strategy PUBLIC ${_quant_strategy_spdlog_target})
unset(_quant_strategy_spdlog_target)

# ------------------------------------------------------------------------------
# nlohmann_json
# ------------------------------------------------------------------------------
if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(quant_strategy PUBLIC nlohmann_json::nlohmann_json)
else()
    find_package(nlohmann_json 3.11 REQUIRED)
    target_link_libraries(quant_strategy PUBLIC nlohmann_json::nlohmann_json)
endif()

# ------------------------------------------------------------------------------
# quant::common（必需）
# ------------------------------------------------------------------------------
if(TARGET quant::common)
    target_link_libraries(quant_strategy PUBLIC quant::common)
elseif(TARGET quant_common)
    target_link_libraries(quant_strategy PUBLIC quant_common)
else()
    message(FATAL_ERROR
        "strategy 模块需要 quant::common，请先 add_subdirectory(src/common)")
endif()

# ------------------------------------------------------------------------------
# quant::indicator（必需）
# ------------------------------------------------------------------------------
if(TARGET quant::indicator)
    target_link_libraries(quant_strategy PUBLIC quant::indicator)
elseif(TARGET quant_indicator)
    target_link_libraries(quant_strategy PUBLIC quant_indicator)
else()
    message(FATAL_ERROR
        "strategy 模块需要 quant::indicator，请先 add_subdirectory(src/indicator)")
endif()

# ------------------------------------------------------------------------------
# quant::ai（可选）
# ------------------------------------------------------------------------------
if(QUANT_STRATEGY_ENABLE_AI)
    if(TARGET quant::ai)
        target_link_libraries(quant_strategy PUBLIC quant::ai)
        target_compile_definitions(quant_strategy PUBLIC QUANT_HAS_AI=1)
        message(STATUS "strategy: AI 集成已启用")
    elseif(TARGET quant_ai)
        target_link_libraries(quant_strategy PUBLIC quant_ai)
        target_compile_definitions(quant_strategy PUBLIC QUANT_HAS_AI=1)
        message(STATUS "strategy: AI 集成已启用（旧目标名）")
    else()
        target_compile_definitions(quant_strategy PUBLIC QUANT_HAS_AI=0)
        message(STATUS "strategy: AI 模块未找到，使用降级实现")
    endif()
else()
    target_compile_definitions(quant_strategy PUBLIC QUANT_HAS_AI=0)
    message(STATUS "strategy: AI 集成已禁用")
endif()

# ------------------------------------------------------------------------------
# prometheus-cpp（可选，统计指标）
# ------------------------------------------------------------------------------
find_package(prometheus-cpp QUIET)
if(prometheus-cpp_FOUND)
    target_link_libraries(quant_strategy PRIVATE
        prometheus-cpp::core
        prometheus-cpp::pull
    )
    target_compile_definitions(quant_strategy PUBLIC QUANT_HAS_PROMETHEUS=1)
    message(STATUS "strategy: prometheus-cpp 已启用")
else()
    target_compile_definitions(quant_strategy PUBLIC QUANT_HAS_PROMETHEUS=0)
endif()

# ==============================================================================
# 14. Windows DLL 导出
# ==============================================================================
if(WIN32 AND BUILD_SHARED_LIBS)
    target_compile_definitions(quant_strategy PUBLIC QUANT_STRATEGY_EXPORTS)
    set_target_properties(quant_strategy PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS ON
    )
endif()

# ==============================================================================
# 15. Debug/Release 输出名区分（Windows）
# ==============================================================================
if(WIN32 AND QUANT_STRATEGY_ENABLE_DEBUG_POSTFIX)
    set_target_properties(quant_strategy PROPERTIES
        DEBUG_POSTFIX "d"
    )
endif()

# ==============================================================================
# 16. 预编译头（PCH）
# ==============================================================================
if(QUANT_ENABLE_PCH AND COMMAND target_precompile_headers)
    target_precompile_headers(quant_strategy PRIVATE
        <algorithm>
        <array>
        <atomic>
        <chrono>
        <cmath>
        <cstddef>
        <cstdint>
        <cstring>
        <deque>
        <functional>
        <limits>
        <map>
        <memory>
        <mutex>
        <optional>
        <random>
        <shared_mutex>
        <span>
        <string>
        <string_view>
        <thread>
        <unordered_map>
        <unordered_set>
        <utility>
        <variant>
        <vector>
    )
    message(STATUS "strategy: PCH 已启用")
endif()

# ==============================================================================
# 17. 契约文件自动化（复制 + 哈希注入）
# ==============================================================================
if(EXISTS "${QUANT_STRATEGY_CONTRACT_FILE}")
    add_custom_command(TARGET quant_strategy PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${CMAKE_BINARY_DIR}/contracts"
    )

    add_custom_command(TARGET quant_strategy POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${QUANT_STRATEGY_CONTRACT_FILE}"
            "${CMAKE_BINARY_DIR}/contracts/strategy.core.contract.json"
        COMMENT "复制 strategy 契约文件"
    )

    # 计算并注入 SHA-256
    if(QUANT_STRATEGY_ENABLE_CONTRACT_HASH)
        file(SHA256 "${QUANT_STRATEGY_CONTRACT_FILE}"
             _quant_strategy_contract_hash)

        add_custom_command(TARGET quant_strategy POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DCONTRACT_FILE=${CMAKE_BINARY_DIR}/contracts/strategy.core.contract.json
                -DCONTRACT_HASH=${_quant_strategy_contract_hash}
                -P "${CMAKE_SOURCE_DIR}/cmake/scripts/inject_contract_hash.cmake"
            COMMENT "注入 strategy 契约哈希"
        )
    endif()
else()
    message(WARNING
        "strategy 契约文件不存在: ${QUANT_STRATEGY_CONTRACT_FILE}")
endif()

# ==============================================================================
# 18. 安装规则
# ==============================================================================
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# 安装静态库
install(TARGETS quant_strategy
    EXPORT quant-strategy-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# 安装头文件
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/strategy
    FILES_MATCHING
        PATTERN "*.hpp"
        PATTERN "*.h"
        PATTERN "*.contract.json"
        PATTERN "CMakeLists.txt" EXCLUDE
)

# 导出目标
install(EXPORT quant-strategy-targets
    FILE quant-strategy-targets.cmake
    NAMESPACE quant::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/quant-strategy
)

# 生成配置文件
configure_package_config_file(
    "${CMAKE_SOURCE_DIR}/cmake/quant-strategy-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/quant-strategy-config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/quant-strategy
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/quant-strategy-config-version.cmake"
    VERSION ${QUANT_STRATEGY_MODULE_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/quant-strategy-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/quant-strategy-config-version.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/quant-strategy
)

# ==============================================================================
# 19. 目标属性
# ==============================================================================
set_target_properties(quant_strategy PROPERTIES
    OUTPUT_NAME "quant_strategy"
    FOLDER "modules"
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    DESCRIPTION "Strategy engine and risk management for quant trading system"
)

# ==============================================================================
# 20. 测试注册（预留）
# ==============================================================================
if(QUANT_BUILD_TESTS AND EXISTS "${CMAKE_SOURCE_DIR}/tests/strategy")
    add_subdirectory("${CMAKE_SOURCE_DIR}/tests/strategy"
                      "${CMAKE_BINARY_DIR}/tests/strategy")
endif()

# ==============================================================================
# 21. 编译摘要
# ==============================================================================
message(STATUS "")
message(STATUS "╔══════════════════════════════════════════════════════════════╗")
message(STATUS "║  strategy 模块配置摘要                                        ║")
message(STATUS "╠══════════════════════════════════════════════════════════════╣")
message(STATUS "║  版本:             ${QUANT_STRATEGY_MODULE_VERSION}")
message(STATUS "║  必需源文件数:     16")
message(STATUS "║  头文件数:         16")
message(STATUS "║  契约文件:         strategy.core.contract.json")
message(STATUS "║  编译器:           ${QUANT_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "╠══════════════════════════════════════════════════════════════╣")
message(STATUS "║  AI 集成:          ${QUANT_STRATEGY_ENABLE_AI}")
message(STATUS "║  Prometheus:       ${prometheus-cpp_FOUND}")
message(STATUS "║  PCH:              ${QUANT_ENABLE_PCH}")
message(STATUS "║  Debug Postfix:    ${QUANT_STRATEGY_ENABLE_DEBUG_POSTFIX}")
message(STATUS "║  契约哈希注入:     ${QUANT_STRATEGY_ENABLE_CONTRACT_HASH}")
message(STATUS "╚══════════════════════════════════════════════════════════════╝")
message(STATUS "")
