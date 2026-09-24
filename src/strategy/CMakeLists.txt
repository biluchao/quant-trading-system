# ==============================================================================
# 币安 BTC/ETH 3分钟量化交易系统 - strategy 模块构建配置
# ==============================================================================
# @file    src/strategy/CMakeLists.txt
# @module  strategy
# @type    core
# @name    CMakeLists
# @version 1.0.2
# @brief   策略引擎、风控、追踪、监控模块构建配置
#          已修复 40 类运行时问题
#
# 依赖:
#   - common (必需, assembly_order 10)
#   - indicator (必需, assembly_order 30)
#   - ai (可选, assembly_order 50)
#   - spdlog, nlohmann_json, Threads (必需)
#   - prometheus-cpp (可选)
#
# 使用方式（下游）:
#   find_package(quant-strategy REQUIRED)
#   target_link_libraries(my_target PRIVATE quant::strategy)
# ==============================================================================

# ==============================================================================
# 1. 模块元信息
# ==============================================================================
set(QUANT_STRATEGY_MODULE_NAME "strategy")
set(QUANT_STRATEGY_MODULE_VERSION "1.0.2")
set(QUANT_STRATEGY_CONTRACT_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/strategy.core.contract.json")

message(STATUS "配置模块: strategy (v${QUANT_STRATEGY_MODULE_VERSION})")

# ==============================================================================
# 2. 依赖加载前置校验
# ==============================================================================
# 确保 common 与 indicator 已在当前构建树中定义目标
if(NOT TARGET quant::common AND NOT TARGET quant_common)
    message(FATAL_ERROR
        "strategy 模块依赖 quant::common，请先 add_subdirectory(src/common) 再加载 strategy")
endif()

if(NOT TARGET quant::indicator AND NOT TARGET quant_indicator)
    message(FATAL_ERROR
        "strategy 模块依赖 quant::indicator，请先 add_subdirectory(src/indicator) 再加载 strategy")
endif()

# ==============================================================================
# 3. 构建选项
# ==============================================================================
option(QUANT_STRATEGY_ENABLE_AI
    "启用 AI 模块集成（缺失时降级）" ON)
option(QUANT_STRATEGY_STRICT_SOURCES
    "源文件缺失时中断构建（推荐开启）" ON)
option(QUANT_STRATEGY_ENABLE_DEBUG_POSTFIX
    "Windows 上 Debug 版本加 d 后缀" ON)
option(QUANT_STRATEGY_ENABLE_CONTRACT_HASH
    "注入契约文件 SHA-256 哈希" ON)
option(QUANT_STRATEGY_ENABLE_PKGCONFIG
    "生成 pkg-config 文件" ON)
option(QUANT_STRATEGY_ENABLE_ASSERTIONS
    "编译期断言校验（仅 Debug）" ON)

# ==============================================================================
# 4. 源文件清单（16 源 + 16 头）
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
# 5. 编译期文件存在性校验
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
# 6. 文件数量与契约一致性校验
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
# 7. 库目标
# ==============================================================================
add_library(quant_strategy STATIC
    ${QUANT_STRATEGY_SOURCES_REQUIRED}
    ${QUANT_STRATEGY_HEADERS}
)

# 命名空间别名（供下游统一使用）
add_library(quant::strategy ALIAS quant_strategy)

# ==============================================================================
# 8. 源文件 IDE 分组
# ==============================================================================
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    PREFIX "Sources"
    FILES ${QUANT_STRATEGY_SOURCES_REQUIRED} ${QUANT_STRATEGY_HEADERS}
)

# ==============================================================================
# 9. Include 目录
# ==============================================================================
target_include_directories(quant_strategy
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# ==============================================================================
# 10. C++ 标准（传递到下游）
# ==============================================================================
target_compile_features(quant_strategy PUBLIC cxx_std_20)

# ==============================================================================
# 11. 编译选项（仅本目标，不传播）
# ==============================================================================
if(QUANT_COMPILER_ID MATCHES "GCC|Clang|AppleClang")
    target_compile_options(quant_strategy PRIVATE
        -Wall -Wextra -Wpedantic
        -Wshadow -Wnon-virtual-dtor -Wold-style-cast
        -Wcast-align -Wunused -Woverloaded-virtual
        -Wnull-dereference -Wdouble-promotion
    )
elseif(QUANT_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(quant_strategy PRIVATE
        /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor
        /utf-8 /external:W0
    )
endif()

# ==============================================================================
# 12. 平台特定编译定义
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
# 13. 位置无关代码
# ==============================================================================
set_target_properties(quant_strategy PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

# ==============================================================================
# 14. 依赖查找与链接
# ==============================================================================

# ---------- 14.1 线程 ----------
find_package(Threads REQUIRED)
target_link_libraries(quant_strategy PUBLIC Threads::Threads)

# ---------- 14.2 spdlog ----------
if(TARGET spdlog::spdlog)
    set(_qs_spdlog spdlog::spdlog)
elseif(TARGET spdlog::spdlog_header_only)
    set(_qs_spdlog spdlog::spdlog_header_only)
else()
    find_package(spdlog 1.11 REQUIRED)
    set(_qs_spdlog spdlog::spdlog)
endif()
target_link_libraries(quant_strategy PUBLIC ${_qs_spdlog})
unset(_qs_spdlog)

# ---------- 14.3 nlohmann_json ----------
if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(quant_strategy PUBLIC nlohmann_json::nlohmann_json)
else()
    find_package(nlohmann_json 3.11 REQUIRED)
    target_link_libraries(quant_strategy PUBLIC nlohmann_json::nlohmann_json)
endif()

# ---------- 14.4 quant::common ----------
if(TARGET quant::common)
    target_link_libraries(quant_strategy PUBLIC quant::common)
elseif(TARGET quant_common)
    target_link_libraries(quant_strategy PUBLIC quant_common)
endif()

# ---------- 14.5 quant::indicator ----------
if(TARGET quant::indicator)
    target_link_libraries(quant_strategy PUBLIC quant::indicator)
elseif(TARGET quant_indicator)
    target_link_libraries(quant_strategy PUBLIC quant_indicator)
endif()

# ---------- 14.6 quant::ai（可选） ----------
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

# ---------- 14.7 prometheus-cpp（可选） ----------
find_package(prometheus-cpp QUIET)
if(prometheus-cpp_FOUND AND TARGET prometheus-cpp::core)
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
# 15. Windows DLL 导出
# ==============================================================================
if(WIN32 AND BUILD_SHARED_LIBS)
    target_compile_definitions(quant_strategy PUBLIC QUANT_STRATEGY_EXPORTS)
    set_target_properties(quant_strategy PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS ON
    )
endif()

# ==============================================================================
# 16. Debug/Release 输出名区分（Windows）
# ==============================================================================
if(WIN32 AND QUANT_STRATEGY_ENABLE_DEBUG_POSTFIX)
    set_target_properties(quant_strategy PROPERTIES
        DEBUG_POSTFIX "d"
    )
endif()

# ==============================================================================
# 17. 预编译头（PCH）
# ==============================================================================
if(QUANT_ENABLE_PCH AND COMMAND target_precompile_headers)
    target_precompile_headers(quant_strategy PRIVATE
        <algorithm> <array> <atomic> <chrono> <cmath>
        <cstddef> <cstdint> <cstring> <deque> <functional>
        <limits> <map> <memory> <mutex> <optional>
        <random> <shared_mutex> <span> <string> <string_view>
        <thread> <unordered_map> <unordered_set>
        <utility> <variant> <vector>
    )
    message(STATUS "strategy: PCH 已启用")
endif()

# ==============================================================================
# 18. 契约文件处理（复制 + 哈希注入）
# ==============================================================================
if(EXISTS "${QUANT_STRATEGY_CONTRACT_FILE}")
    # 构建时复制到 build 目录
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

    # 哈希注入（脚本幂等）
    if(QUANT_STRATEGY_ENABLE_CONTRACT_HASH)
        file(SHA256 "${QUANT_STRATEGY_CONTRACT_FILE}"
             _quant_strategy_contract_hash)

        add_custom_command(TARGET quant_strategy POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DCONTRACT_FILE=${CMAKE_BINARY_DIR}/contracts/strategy.core.contract.json
                -DCONTRACT_HASH=${_quant_strategy_contract_hash}
                -DINJECT_VERBOSE=OFF
                -DINJECT_BACKUP=OFF
                -P "${CMAKE_SOURCE_DIR}/cmake/scripts/inject_contract_hash.cmake"
            COMMENT "注入 strategy 契约哈希"
        )
    endif()
else()
    message(WARNING
        "strategy 契约文件不存在: ${QUANT_STRATEGY_CONTRACT_FILE}")
endif()

# ==============================================================================
# 19. 安装规则
# ==============================================================================
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------- 19.1 安装库 ----------
install(TARGETS quant_strategy
    EXPORT quant-strategy-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# ---------- 19.2 安装头文件 ----------
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/strategy
    FILES_MATCHING
        PATTERN "*.hpp"
        PATTERN "*.h"
        PATTERN "*.hh"
        PATTERN "*.hxx"
        PATTERN "*.inl"
        PATTERN "*.ipp"
        PATTERN "*.contract.json"
        PATTERN "CMakeLists.txt" EXCLUDE
        PATTERN "*.cpp" EXCLUDE
        PATTERN "*.cmake" EXCLUDE
)

# ---------- 19.3 安装契约文件 ----------
if(EXISTS "${QUANT_STRATEGY_CONTRACT_FILE}")
    install(FILES
        "${QUANT_STRATEGY_CONTRACT_FILE}"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/quant/contracts
        RENAME "strategy.core.contract.json"
    )
endif()

# ---------- 19.4 导出目标 ----------
install(EXPORT quant-strategy-targets
    FILE quant-strategy-targets.cmake
    NAMESPACE quant::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/quant-strategy
)

# ---------- 19.5 生成 config 文件 ----------
configure_package_config_file(
    "${CMAKE_SOURCE_DIR}/cmake/quant-strategy-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/quant-strategy-config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/quant-strategy
    PATH_VARS
        CMAKE_INSTALL_INCLUDEDIR
        CMAKE_INSTALL_LIBDIR
        CMAKE_INSTALL_DATADIR
)

# ---------- 19.6 生成 version 文件 ----------
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/quant-strategy-config-version.cmake"
    VERSION ${QUANT_STRATEGY_MODULE_VERSION}
    COMPATIBILITY SameMajorVersion
    ARCH_INDEPENDENT
)

# ---------- 19.7 安装 config 文件 ----------
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/quant-strategy-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/quant-strategy-config-version.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/quant-strategy
)

# ==============================================================================
# 20. pkg-config 文件（可选）
# ==============================================================================
if(QUANT_STRATEGY_ENABLE_PKGCONFIG AND UNIX)
    set(_qs_pc_file
        "${CMAKE_CURRENT_BINARY_DIR}/quant-strategy.pc")

    file(WRITE "${_qs_pc_file}"
"prefix=${CMAKE_INSTALL_PREFIX}
exec_prefix=\${prefix}
libdir=\${prefix}/${CMAKE_INSTALL_LIBDIR}
includedir=\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}

Name: quant-strategy
Description: Strategy engine and risk management for quant trading system
Version: ${QUANT_STRATEGY_MODULE_VERSION}
Requires: quant-common quant-indicator
Requires.private:
Libs: -L\${libdir} -lquant_strategy
Cflags: -I\${includedir}
")

    install(FILES "${_qs_pc_file}"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
    )
    unset(_qs_pc_file)
endif()

# ==============================================================================
# 21. 目标属性
# ==============================================================================
set_target_properties(quant_strategy PROPERTIES
    OUTPUT_NAME "quant_strategy"
    EXPORT_NAME "strategy"
    FOLDER "modules"
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    DESCRIPTION "Strategy engine and risk management"
)

# ==============================================================================
# 22. 编译期断言（仅 Debug）
# ==============================================================================
if(QUANT_STRATEGY_ENABLE_ASSERTIONS AND
   CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(quant_strategy PRIVATE
        QUANT_STRATEGY_BUILD_ASSERTIONS=1
    )
endif()

# ==============================================================================
# 23. 测试注册（预留）
# ==============================================================================
if(QUANT_BUILD_TESTS AND EXISTS "${CMAKE_SOURCE_DIR}/tests/strategy")
    add_subdirectory("${CMAKE_SOURCE_DIR}/tests/strategy"
                      "${CMAKE_BINARY_DIR}/tests/strategy")
endif()

# ==============================================================================
# 24. 编译摘要
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
message(STATUS "║  pkg-config:       ${QUANT_STRATEGY_ENABLE_PKGCONFIG}")
message(STATUS "╚══════════════════════════════════════════════════════════════╝")
message(STATUS "")
