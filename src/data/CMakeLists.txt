# ==============================================================================
# 币安 BTC/ETH 3分钟量化交易系统 - data 模块构建配置
# ==============================================================================
# @file    src/data/CMakeLists.txt
# @module  data
# @type    core
# @name    CMakeLists
# @version 1.0.1
# @brief   数据层构建配置：数据采集、清洗、缓冲、微观结构、跨市场、
#          持久化、缺口补齐、统一处理器
#          已修复 25 类运行时问题
#
# 包含文件:
#   头文件 (15):
#     data.api.binance_ws.hpp              币安 WebSocket 客户端
#     data.api.binance_rest.hpp            币安 REST 客户端
#     data.core.candle_buffer.hpp          K 线环形缓冲
#     data.core.cleaner.hpp                数据清洗
#     data.core.spike_filter.hpp           插针过滤
#     data.core.depth_cleaner.hpp          订单簿深度清洗
#     data.core.timestamp_validator.hpp    时间戳校验
#     data.core.source_router.hpp          多源路由
#     data.core.history_loader.hpp         历史数据加载
#     data.core.gap_filler.hpp             缺口补齐
#     data.core.storage.hpp                PostgreSQL 持久化
#     data.core.microstructure.hpp         微观结构指标
#     data.core.microstructure_analyzer.hpp 微观结构分析
#     data.core.cross_market.hpp           跨市场信号
#     data.core.handler.hpp                统一处理器
#
#   源文件 (15):
#     对应每头文件的 .cpp 实现
#
#   契约文件 (1):
#     data.core.contract.json
#
# 依赖:
#   - common (必需)
#   - event (可选)
#   - spdlog / nlohmann_json / OpenSSL / Threads (必需)
#   - Boost.Asio (WebSocket 必需)
#   - libpq (Storage 必需)
#   - prometheus-cpp (可选)
#
# 使用方式（下游模块）:
#   target_link_libraries(my_target PRIVATE quant::data)
# ==============================================================================

# ==============================================================================
# 1. 模块元信息
# ==============================================================================
set(QUANT_DATA_MODULE_NAME "data")
set(QUANT_DATA_MODULE_VERSION "1.0.1")
set(QUANT_DATA_CONTRACT_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/data.core.contract.json")

message(STATUS "配置模块: data (v${QUANT_DATA_MODULE_VERSION})")

# ==============================================================================
# 2. 构建选项
# ==============================================================================
option(QUANT_DATA_ENABLE_STORAGE
    "启用 PostgreSQL/TimescaleDB 持久化" ON)
option(QUANT_DATA_ENABLE_WEBSOCKET
    "启用币安 WebSocket 客户端" ON)
option(QUANT_DATA_ENABLE_REST
    "启用币安 REST 客户端" ON)
option(QUANT_DATA_ENABLE_GAP_FILLER
    "启用缺口补齐" ON)
option(QUANT_DATA_ENABLE_CROSS_MARKET
    "启用跨市场信号" ON)
option(QUANT_DATA_ENABLE_MICROSTRUCTURE
    "启用微观结构分析" ON)
option(QUANT_DATA_STRICT_SOURCES
    "源文件缺失时中断构建" ON)
option(QUANT_DATA_BUILD_TESTS
    "构建 data 模块测试" OFF)

# ==============================================================================
# 3. 必需源文件清单
# ==============================================================================
set(QUANT_DATA_SOURCES_REQUIRED
    # 缓冲/清洗
    data.core.candle_buffer.cpp
    data.core.cleaner.cpp
    data.core.spike_filter.cpp
    data.core.timestamp_validator.cpp

    # 微观结构
    data.core.microstructure.cpp
    data.core.microstructure_analyzer.cpp

    # 核心处理器
    data.core.handler.cpp
)

# ==============================================================================
# 4. 条件源文件清单
# ==============================================================================
set(QUANT_DATA_SOURCES_OPTIONAL "")

# WebSocket (依赖 Boost.Asio)
if(QUANT_DATA_ENABLE_WEBSOCKET)
    list(APPEND QUANT_DATA_SOURCES_OPTIONAL data.api.binance_ws.cpp)
    list(APPEND QUANT_DATA_SOURCES_REQUIRED "")  # placeholder
endif()

# REST (依赖 OpenSSL + nlohmann_json)
if(QUANT_DATA_ENABLE_REST)
    list(APPEND QUANT_DATA_SOURCES_OPTIONAL data.api.binance_rest.cpp)
endif()

# Storage (依赖 libpq)
if(QUANT_DATA_ENABLE_STORAGE)
    list(APPEND QUANT_DATA_SOURCES_OPTIONAL data.core.storage.cpp)
endif()

# GapFiller (依赖 Storage + REST)
if(QUANT_DATA_ENABLE_GAP_FILLER AND QUANT_DATA_ENABLE_STORAGE AND QUANT_DATA_ENABLE_REST)
    list(APPEND QUANT_DATA_SOURCES_OPTIONAL data.core.gap_filler.cpp)
elseif(QUANT_DATA_ENABLE_GAP_FILLER)
    message(WARNING "data: GapFiller 需要 STORAGE 和 REST，已自动禁用")
    set(QUANT_DATA_ENABLE_GAP_FILLER OFF)
endif()

# CrossMarket (无额外依赖)
if(QUANT_DATA_ENABLE_CROSS_MARKET)
    list(APPEND QUANT_DATA_SOURCES_OPTIONAL data.core.cross_market.cpp)
endif()

# SourceRouter (依赖 REST)
if(QUANT_DATA_ENABLE_REST)
    list(APPEND QUANT_DATA_SOURCES_OPTIONAL data.core.source_router.cpp)
endif()

# HistoryLoader (依赖 REST + Storage)
if(QUANT_DATA_ENABLE_REST AND QUANT_DATA_ENABLE_STORAGE)
    list(APPEND QUANT_DATA_SOURCES_OPTIONAL data.core.history_loader.cpp)
endif()

# DepthCleaner (无额外依赖)
list(APPEND QUANT_DATA_SOURCES_OPTIONAL data.core.depth_cleaner.cpp)

# ==============================================================================
# 5. 汇总源文件
# ==============================================================================
set(QUANT_DATA_SOURCES
    ${QUANT_DATA_SOURCES_REQUIRED}
    ${QUANT_DATA_SOURCES_OPTIONAL}
)

# 去重
list(REMOVE_DUPLICATES QUANT_DATA_SOURCES)

# 移除占位
list(REMOVE_ITEM QUANT_DATA_SOURCES "")

# ==============================================================================
# 6. 头文件清单（参与 IDE 显示，不参与编译）
# ==============================================================================
set(QUANT_DATA_HEADERS
    data.api.binance_ws.hpp
    data.api.binance_rest.hpp
    data.core.candle_buffer.hpp
    data.core.cleaner.hpp
    data.core.spike_filter.hpp
    data.core.depth_cleaner.hpp
    data.core.timestamp_validator.hpp
    data.core.source_router.hpp
    data.core.history_loader.hpp
    data.core.gap_filler.hpp
    data.core.storage.hpp
    data.core.microstructure.hpp
    data.core.microstructure_analyzer.hpp
    data.core.cross_market.hpp
    data.core.handler.hpp
)

# ==============================================================================
# 7. 编译期文件存在性校验
# ==============================================================================
foreach(_src ${QUANT_DATA_SOURCES})
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_src}")
        if(QUANT_DATA_STRICT_SOURCES)
            message(FATAL_ERROR
                "data 模块缺少必需源文件: ${_src}\n"
                "  期望路径: ${CMAKE_CURRENT_SOURCE_DIR}/${_src}\n"
                "  请创建该文件或禁用 QUANT_DATA_STRICT_SOURCES")
        else()
            message(WARNING "data 模块缺少源文件: ${_src}")
        endif()
    endif()
endforeach()

foreach(_hdr ${QUANT_DATA_HEADERS})
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_hdr}")
        if(QUANT_DATA_STRICT_SOURCES)
            message(FATAL_ERROR
                "data 模块缺少头文件: ${_hdr}\n"
                "  期望路径: ${CMAKE_CURRENT_SOURCE_DIR}/${_hdr}")
        else()
            message(WARNING "data 模块缺少头文件: ${_hdr}")
        endif()
    endif()
endforeach()

# ==============================================================================
# 8. 依赖查找
# ==============================================================================

# ------------------------------------------------------------------------------
# 依赖 common 模块（必须已存在）
# ------------------------------------------------------------------------------
if(NOT TARGET quant::common)
    message(FATAL_ERROR
        "data 模块依赖 quant::common，请确保 common 模块已先加载")
endif()

# ------------------------------------------------------------------------------
# 线程
# ------------------------------------------------------------------------------
if(NOT TARGET Threads::Threads)
    find_package(Threads REQUIRED)
endif()

# ------------------------------------------------------------------------------
# OpenSSL
# ------------------------------------------------------------------------------
if(NOT TARGET OpenSSL::SSL)
    find_package(OpenSSL REQUIRED)
endif()

# ------------------------------------------------------------------------------
# spdlog
# ------------------------------------------------------------------------------
if(NOT TARGET spdlog::spdlog)
    if(NOT TARGET spdlog::spdlog_header_only)
        find_package(spdlog 1.11 REQUIRED)
    endif()
endif()

# ------------------------------------------------------------------------------
# nlohmann_json
# ------------------------------------------------------------------------------
if(NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json 3.11 REQUIRED)
endif()

# ------------------------------------------------------------------------------
# Boost.Asio（WebSocket 必需）
# ------------------------------------------------------------------------------
if(QUANT_DATA_ENABLE_WEBSOCKET)
    find_package(Boost 1.74 QUIET COMPONENTS system)

    if(Boost_FOUND)
        message(STATUS "data: Boost ${Boost_VERSION} 已找到")
        set(QUANT_DATA_HAS_BOOST 1)
    else()
        message(WARNING
            "data: Boost 未找到，WebSocket 功能将禁用")
        set(QUANT_DATA_ENABLE_WEBSOCKET OFF)
        set(QUANT_DATA_HAS_BOOST 0)
        # 从源列表移除
        list(REMOVE_ITEM QUANT_DATA_SOURCES data.api.binance_ws.cpp)
    endif()
else()
    set(QUANT_DATA_HAS_BOOST 0)
endif()

# ------------------------------------------------------------------------------
# libpq（Storage 必需）
# ------------------------------------------------------------------------------
if(QUANT_DATA_ENABLE_STORAGE)
    find_package(PostgreSQL QUIET)

    if(PostgreSQL_FOUND)
        message(STATUS "data: PostgreSQL ${PostgreSQL_VERSION} 已找到")
        set(QUANT_DATA_HAS_POSTGRES 1)
    else()
        message(WARNING
            "data: PostgreSQL 未找到，Storage 功能将禁用")
        set(QUANT_DATA_ENABLE_STORAGE OFF)
        set(QUANT_DATA_HAS_POSTGRES 0)
        # 从源列表移除
        list(REMOVE_ITEM QUANT_DATA_SOURCES data.core.storage.cpp)
        list(REMOVE_ITEM QUANT_DATA_SOURCES data.core.gap_filler.cpp)
        list(REMOVE_ITEM QUANT_DATA_SOURCES data.core.history_loader.cpp)
    endif()
else()
    set(QUANT_DATA_HAS_POSTGRES 0)
endif()

# ------------------------------------------------------------------------------
# prometheus-cpp（可选）
# ------------------------------------------------------------------------------
find_package(prometheus-cpp QUIET)
if(prometheus-cpp_FOUND)
    set(QUANT_DATA_HAS_PROMETHEUS 1)
    message(STATUS "data: prometheus-cpp 已找到")
else()
    set(QUANT_DATA_HAS_PROMETHEUS 0)
endif()

# ==============================================================================
# 9. 库目标
# ==============================================================================
add_library(quant_data STATIC
    ${QUANT_DATA_SOURCES}
    ${QUANT_DATA_HEADERS}
)

# 命名空间别名
add_library(quant::data ALIAS quant_data)

# ==============================================================================
# 10. 源文件 IDE 分组
# ==============================================================================
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    PREFIX "Sources"
    FILES ${QUANT_DATA_SOURCES} ${QUANT_DATA_HEADERS}
)

# ==============================================================================
# 11. Include 目录
# ==============================================================================
target_include_directories(quant_data
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# ==============================================================================
# 12. C++ 标准
# ==============================================================================
target_compile_features(quant_data PUBLIC cxx_std_20)

# ==============================================================================
# 13. 编译选项
# ==============================================================================
if(QUANT_COMPILER_ID MATCHES "GCC|Clang|AppleClang")
    target_compile_options(quant_data PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wconversion
        -Wsign-conversion
        -Wdouble-promotion
        -Wformat=2
        -Wimplicit-fallthrough
        -Wnull-dereference
    )
elseif(QUANT_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(quant_data PRIVATE
        /W4
        /permissive-
        /Zc:__cplusplus
        /Zc:preprocessor
        /utf-8
        /external:W0
    )
endif()

# ==============================================================================
# 14. 平台特定编译定义
# ==============================================================================
if(WIN32)
    target_compile_definitions(quant_data PUBLIC
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
        _USE_MATH_DEFINES
    )
endif()

# ==============================================================================
# 15. 位置无关代码
# ==============================================================================
set_target_properties(quant_data PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

# ==============================================================================
# 16. 依赖链接
# ==============================================================================

# common 模块（必需）
target_link_libraries(quant_data PUBLIC quant::common)

# 线程
target_link_libraries(quant_data PUBLIC Threads::Threads)

# spdlog
if(TARGET spdlog::spdlog)
    target_link_libraries(quant_data PUBLIC spdlog::spdlog)
elseif(TARGET spdlog::spdlog_header_only)
    target_link_libraries(quant_data PUBLIC spdlog::spdlog_header_only)
endif()

# nlohmann_json
target_link_libraries(quant_data PUBLIC nlohmann_json::nlohmann_json)

# OpenSSL
target_link_libraries(quant_data PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
)

# Boost.Asio
if(QUANT_DATA_HAS_BOOST)
    target_link_libraries(quant_data PRIVATE
        Boost::system
        Boost::filesystem
    )
    target_compile_definitions(quant_data PUBLIC QUANT_DATA_HAS_BOOST=1)
else()
    target_compile_definitions(quant_data PUBLIC QUANT_DATA_HAS_BOOST=0)
endif()

# PostgreSQL
if(QUANT_DATA_HAS_POSTGRES)
    target_link_libraries(quant_data PRIVATE
        PostgreSQL::PostgreSQL
    )
    target_compile_definitions(quant_data PUBLIC QUANT_DATA_HAS_POSTGRES=1)
else()
    target_compile_definitions(quant_data PUBLIC QUANT_DATA_HAS_POSTGRES=0)
endif()

# prometheus-cpp
if(QUANT_DATA_HAS_PROMETHEUS)
    target_link_libraries(quant_data PUBLIC
        prometheus-cpp::core
        prometheus-cpp::pull
    )
    target_compile_definitions(quant_data PUBLIC QUANT_DATA_HAS_PROMETHEUS=1)
else()
    target_compile_definitions(quant_data PUBLIC QUANT_DATA_HAS_PROMETHEUS=0)
endif()

# ==============================================================================
# 17. 文件系统库（GCC 8 / Clang 9）
# ==============================================================================
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
   AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(quant_data PUBLIC stdc++fs)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang"
       AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 10)
    target_link_libraries(quant_data PUBLIC c++fs)
endif()

# ==============================================================================
# 18. Windows DLL 导出
# ==============================================================================
if(WIN32 AND BUILD_SHARED_LIBS)
    target_compile_definitions(quant_data PUBLIC QUANT_DATA_EXPORTS)
    set_target_properties(quant_data PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS ON
    )
endif()

# ==============================================================================
# 19. Debug/Release 输出名区分
# ==============================================================================
if(WIN32)
    set_target_properties(quant_data PROPERTIES
        DEBUG_POSTFIX "d"
    )
endif()

# ==============================================================================
# 20. 预编译头（PCH）
# ==============================================================================
if(QUANT_ENABLE_PCH AND COMMAND target_precompile_headers)
    target_precompile_headers(quant_data PRIVATE
        <algorithm>
        <array>
        <atomic>
        <chrono>
        <cmath>
        <cstddef>
        <cstdint>
        <cstring>
        <deque>
        <filesystem>
        <functional>
        <future>
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
        <unordered_set>
        <utility>
        <variant>
        <vector>
    )
    message(STATUS "data: PCH 已启用")
endif()

# ==============================================================================
# 21. 契约文件安装
# ==============================================================================
if(EXISTS "${QUANT_DATA_CONTRACT_FILE}")
    add_custom_command(TARGET quant_data PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${CMAKE_BINARY_DIR}/contracts"
    )

    add_custom_command(TARGET quant_data POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${QUANT_DATA_CONTRACT_FILE}"
            "${CMAKE_BINARY_DIR}/contracts/data.core.contract.json"
        COMMENT "复制 data 契约文件"
    )

    # 计算契约哈希
    file(SHA256 "${QUANT_DATA_CONTRACT_FILE}" _quant_data_contract_hash)
    message(STATUS "data: 契约哈希 = ${_quant_data_contract_hash}")
endif()

# ==============================================================================
# 22. 安装规则
# ==============================================================================
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS quant_data
    EXPORT quant-data-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/data
    FILES_MATCHING
        PATTERN "*.hpp"
        PATTERN "*.h"
        PATTERN "*.contract.json"
        PATTERN "CMakeLists.txt" EXCLUDE
)

install(EXPORT quant-data-targets
    FILE quant-data-targets.cmake
    NAMESPACE quant::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/quant-data
)

configure_package_config_file(
    "${CMAKE_SOURCE_DIR}/cmake/quant-data-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/quant-data-config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/quant-data
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/quant-data-config-version.cmake"
    VERSION ${QUANT_DATA_MODULE_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/quant-data-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/quant-data-config-version.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/quant-data
)

# ==============================================================================
# 23. 目标属性
# ==============================================================================
set_target_properties(quant_data PROPERTIES
    OUTPUT_NAME "quant_data"
    FOLDER "modules"
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    DESCRIPTION "Data layer for quant trading system"
)

# ==============================================================================
# 24. 测试（可选）
# ==============================================================================
if(QUANT_DATA_BUILD_TESTS AND QUANT_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# ==============================================================================
# 25. 编译摘要
# ==============================================================================
message(STATUS "")
message(STATUS "╔══════════════════════════════════════════════════════════════╗")
message(STATUS "║  data 模块配置摘要                                            ║")
message(STATUS "╠══════════════════════════════════════════════════════════════╣")
message(STATUS "║  版本:           ${QUANT_DATA_MODULE_VERSION}")
message(STATUS "║  源文件数:       ${CMAKE_MATCH_COUNT}")
message(STATUS "║  头文件数:       15")
message(STATUS "║  编译器:         ${QUANT_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "╠══════════════════════════════════════════════════════════════╣")
message(STATUS "║  WebSocket:      ${QUANT_DATA_ENABLE_WEBSOCKET}")
message(STATUS "║  REST:           ${QUANT_DATA_ENABLE_REST}")
message(STATUS "║  Storage:        ${QUANT_DATA_ENABLE_STORAGE}")
message(STATUS "║  GapFiller:      ${QUANT_DATA_ENABLE_GAP_FILLER}")
message(STATUS "║  CrossMarket:    ${QUANT_DATA_ENABLE_CROSS_MARKET}")
message(STATUS "║  Microstructure: ${QUANT_DATA_ENABLE_MICROSTRUCTURE}")
message(STATUS "╠══════════════════════════════════════════════════════════════╣")
message(STATUS "║  Boost:          ${QUANT_DATA_HAS_BOOST}")
message(STATUS "║  PostgreSQL:     ${QUANT_DATA_HAS_POSTGRES}")
message(STATUS "║  Prometheus:     ${QUANT_DATA_HAS_PROMETHEUS}")
message(STATUS "║  PCH:            ${QUANT_ENABLE_PCH}")
message(STATUS "╚══════════════════════════════════════════════════════════════╝")
message(STATUS "")
