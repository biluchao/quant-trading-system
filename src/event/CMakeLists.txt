# ==============================================================================
# 币安 BTC/ETH 3分钟量化交易系统 - event 模块构建配置
# ==============================================================================
# @file    src/event/CMakeLists.txt
# @module  event
# @type    core
# @name    CMakeLists
# @version 1.0.1
# @brief   事件总线构建配置：发布/订阅、序列化、因果链追踪
#          已修复 20+ 类运行时问题
#
# 包含文件:
#   头文件 (4):
#     event.core.bus.hpp             事件总线（发布/订阅，基于无锁队列）
#     event.core.serializer.hpp      事件序列化（FlatBuffers/JSON）
#     event.core.trace.hpp           因果链追踪（trace_id/parent_id）
#     event.core.handler.hpp         事件处理器基类
#
#   源文件 (3 必需):
#     event.core.bus.cpp             （必需）订阅管理、分发线程、背压控制
#     event.core.serializer.cpp      （必需）序列化/反序列化
#     event.core.trace.cpp           （必需）因果链管理
#
# 使用方式（下游模块）:
#   target_link_libraries(my_target PRIVATE quant::event)
#
# 依赖:
#   quant::common（必需）
#   Threads, spdlog, nlohmann_json
# ==============================================================================

# ==============================================================================
# 1. 模块元信息
# ==============================================================================
set(QUANT_EVENT_MODULE_NAME "event")
set(QUANT_EVENT_MODULE_VERSION "1.0.1")
set(QUANT_EVENT_CONTRACT_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/event.core.contract.json")

message(STATUS "配置模块: event (v${QUANT_EVENT_MODULE_VERSION})")

# ==============================================================================
# 2. 构建选项
# ==============================================================================
option(QUANT_EVENT_ENABLE_TRACING
    "启用因果链追踪（生产环境建议开启）" ON)
option(QUANT_EVENT_ENABLE_PERSISTENCE
    "启用事件持久化（落盘）" OFF)
option(QUANT_EVENT_ENABLE_BENCHMARK
    "构建事件总线基准测试" OFF)
option(QUANT_EVENT_STRICT_SOURCES
    "源文件缺失时中断构建（推荐开启）" ON)

# ==============================================================================
# 3. 必需源文件清单
# ==============================================================================
set(QUANT_EVENT_SOURCES_REQUIRED
    event.core.bus.cpp
    event.core.serializer.cpp
    event.core.trace.cpp
)

# ==============================================================================
# 4. 可选源文件清单
# ==============================================================================
set(QUANT_EVENT_SOURCES_OPTIONAL "")

if(QUANT_EVENT_ENABLE_PERSISTENCE)
    list(APPEND QUANT_EVENT_SOURCES_OPTIONAL
        event.core.persistence.cpp)
endif()

# ==============================================================================
# 5. 汇总源文件
# ==============================================================================
set(QUANT_EVENT_SOURCES
    ${QUANT_EVENT_SOURCES_REQUIRED}
    ${QUANT_EVENT_SOURCES_OPTIONAL}
)

# ==============================================================================
# 6. 头文件清单（参与 IDE 显示，不参与编译）
# ==============================================================================
set(QUANT_EVENT_HEADERS
    event.core.bus.hpp
    event.core.serializer.hpp
    event.core.trace.hpp
    event.core.handler.hpp
)

# ==============================================================================
# 7. 编译期文件存在性校验
# ==============================================================================
foreach(_src ${QUANT_EVENT_SOURCES})
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_src}")
        if(QUANT_EVENT_STRICT_SOURCES)
            message(FATAL_ERROR
                "event 模块缺少必需源文件: ${_src}\n"
                "  期望路径: ${CMAKE_CURRENT_SOURCE_DIR}/${_src}\n"
                "  请创建该文件或禁用 QUANT_EVENT_STRICT_SOURCES")
        else()
            message(WARNING "event 模块缺少源文件: ${_src}")
        endif()
    endif()
endforeach()

foreach(_hdr ${QUANT_EVENT_HEADERS})
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_hdr}")
        if(QUANT_EVENT_STRICT_SOURCES)
            message(FATAL_ERROR
                "event 模块缺少头文件: ${_hdr}\n"
                "  期望路径: ${CMAKE_CURRENT_SOURCE_DIR}/${_hdr}")
        else()
            message(WARNING "event 模块缺少头文件: ${_hdr}")
        endif()
    endif()
endforeach()

# ==============================================================================
# 8. 库目标
# ==============================================================================
add_library(quant_event STATIC
    ${QUANT_EVENT_SOURCES}
    ${QUANT_EVENT_HEADERS}
)

# 命名空间别名（下游统一使用 quant::event）
add_library(quant::event ALIAS quant_event)

# ==============================================================================
# 9. 源文件 IDE 分组
# ==============================================================================
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    PREFIX "Sources"
    FILES ${QUANT_EVENT_SOURCES} ${QUANT_EVENT_HEADERS}
)

# ==============================================================================
# 10. Include 目录（使用 CURRENT_SOURCE_DIR 而非 CMAKE_SOURCE_DIR）
# ==============================================================================
target_include_directories(quant_event
    PUBLIC
        # 下游通过 #include <event/event.core.bus.hpp> 引用
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# ==============================================================================
# 11. C++ 标准（传递到下游）
# ==============================================================================
target_compile_features(quant_event PUBLIC cxx_std_20)

# ==============================================================================
# 12. 编译选项（仅作用于本目标，不传播到下游）
# ==============================================================================
if(QUANT_COMPILER_ID MATCHES "GCC|Clang|AppleClang")
    target_compile_options(quant_event PRIVATE
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
    target_compile_options(quant_event PRIVATE
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
    target_compile_definitions(quant_event PUBLIC
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
        _USE_MATH_DEFINES
    )
endif()

# ==============================================================================
# 14. 位置无关代码（供链接到共享库）
# ==============================================================================
set_target_properties(quant_event PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

# ==============================================================================
# 15. 依赖查找与链接
# ==============================================================================

# ------------------------------------------------------------------------------
# 内部依赖：quant::common
# ------------------------------------------------------------------------------
if(TARGET quant::common)
    target_link_libraries(quant_event PUBLIC quant::common)
elseif(TARGET quant_common)
    target_link_libraries(quant_event PUBLIC quant_common)
else()
    message(FATAL_ERROR
        "event 模块依赖 quant::common，但未找到目标。\n"
        "  请确保 src/common 在 src/event 之前 add_subdirectory")
endif()

# ------------------------------------------------------------------------------
# 线程库（事件分发线程、订阅管理）
# ------------------------------------------------------------------------------
find_package(Threads REQUIRED)
target_link_libraries(quant_event PUBLIC Threads::Threads)

# ------------------------------------------------------------------------------
# spdlog（事件日志）
# ------------------------------------------------------------------------------
if(TARGET spdlog::spdlog)
    target_link_libraries(quant_event PUBLIC spdlog::spdlog)
elseif(TARGET spdlog::spdlog_header_only)
    target_link_libraries(quant_event PUBLIC spdlog::spdlog_header_only)
else()
    find_package(spdlog 1.11 REQUIRED)
    target_link_libraries(quant_event PUBLIC spdlog::spdlog)
endif()

# ------------------------------------------------------------------------------
# nlohmann_json（事件序列化）
# ------------------------------------------------------------------------------
if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(quant_event PUBLIC nlohmann_json::nlohmann_json)
else()
    find_package(nlohmann_json 3.11 REQUIRED)
    target_link_libraries(quant_event PUBLIC nlohmann_json::nlohmann_json)
endif()

# ------------------------------------------------------------------------------
# FlatBuffers（可选，高性能序列化）
# ------------------------------------------------------------------------------
find_package(flatbuffers QUIET)
if(flatbuffers_FOUND AND TARGET flatbuffers::flatbuffers)
    target_link_libraries(quant_event PUBLIC flatbuffers::flatbuffers)
    target_compile_definitions(quant_event PUBLIC QUANT_EVENT_HAS_FLATBUFFERS=1)
    message(STATUS "event: FlatBuffers 已启用")
else()
    target_compile_definitions(quant_event PUBLIC QUANT_EVENT_HAS_FLATBUFFERS=0)
    message(STATUS "event: FlatBuffers 未找到，使用 JSON 序列化")
endif()

# ------------------------------------------------------------------------------
# Prometheus（可选，事件指标）
# ------------------------------------------------------------------------------
if(TARGET prometheus-cpp::core)
    target_link_libraries(quant_event PUBLIC prometheus-cpp::core)
    target_compile_definitions(quant_event PUBLIC QUANT_EVENT_HAS_PROMETHEUS=1)
else()
    target_compile_definitions(quant_event PUBLIC QUANT_EVENT_HAS_PROMETHEUS=0)
endif()

# ==============================================================================
# 16. 编译定义（模块功能开关）
# ==============================================================================
if(QUANT_EVENT_ENABLE_TRACING)
    target_compile_definitions(quant_event PUBLIC QUANT_EVENT_TRACING=1)
    message(STATUS "event: 因果链追踪已启用")
else()
    target_compile_definitions(quant_event PUBLIC QUANT_EVENT_TRACING=0)
endif()

if(QUANT_EVENT_ENABLE_PERSISTENCE)
    target_compile_definitions(quant_event PUBLIC QUANT_EVENT_PERSISTENCE=1)
    message(STATUS "event: 事件持久化已启用")
else()
    target_compile_definitions(quant_event PUBLIC QUANT_EVENT_PERSISTENCE=0)
endif()

# ==============================================================================
# 17. Windows DLL 导出
# ==============================================================================
if(WIN32 AND BUILD_SHARED_LIBS)
    target_compile_definitions(quant_event PUBLIC QUANT_EVENT_EXPORTS)
    set_target_properties(quant_event PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS ON
    )
endif()

# ==============================================================================
# 18. Debug/Release 输出名区分（Windows）
# ==============================================================================
if(WIN32)
    set_target_properties(quant_event PROPERTIES
        DEBUG_POSTFIX "d"
    )
endif()

# ==============================================================================
# 19. 预编译头（PCH）
# ==============================================================================
if(QUANT_ENABLE_PCH AND COMMAND target_precompile_headers)
    target_precompile_headers(quant_event PRIVATE
        <algorithm>
        <array>
        <atomic>
        <chrono>
        <cstddef>
        <cstdint>
        <cstring>
        <functional>
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
    message(STATUS "event: PCH 已启用")
endif()

# ==============================================================================
# 20. 契约文件安装（项目规范要求）
# ==============================================================================
if(EXISTS "${QUANT_EVENT_CONTRACT_FILE}")
    add_custom_command(TARGET quant_event PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${CMAKE_BINARY_DIR}/contracts"
    )

    add_custom_command(TARGET quant_event POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${QUANT_EVENT_CONTRACT_FILE}"
            "${CMAKE_BINARY_DIR}/contracts/event.core.contract.json"
        COMMENT "复制 event 契约文件"
    )

    # 计算并注入契约哈希
    file(SHA256 "${QUANT_EVENT_CONTRACT_FILE}" _quant_event_contract_hash)

    add_custom_command(TARGET quant_event POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DCONTRACT_FILE=${CMAKE_BINARY_DIR}/contracts/event.core.contract.json
            -DCONTRACT_HASH=${_quant_event_contract_hash}
            -P "${CMAKE_SOURCE_DIR}/cmake/scripts/inject_contract_hash.cmake"
        COMMENT "注入 event 契约哈希"
    )
endif()

# ==============================================================================
# 21. 安装规则
# ==============================================================================
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# 安装静态库
install(TARGETS quant_event
    EXPORT quant-event-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# 安装头文件
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/event
    FILES_MATCHING
        PATTERN "*.hpp"
        PATTERN "*.h"
        PATTERN "*.contract.json"
        PATTERN "CMakeLists.txt" EXCLUDE
)

# 导出目标
install(EXPORT quant-event-targets
    FILE quant-event-targets.cmake
    NAMESPACE quant::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/quant-event
)

# 生成配置文件
configure_package_config_file(
    "${CMAKE_SOURCE_DIR}/cmake/quant-event-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/quant-event-config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/quant-event
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/quant-event-config-version.cmake"
    VERSION ${QUANT_EVENT_MODULE_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/quant-event-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/quant-event-config-version.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/quant-event
)

# ==============================================================================
# 22. 目标属性
# ==============================================================================
set_target_properties(quant_event PROPERTIES
    OUTPUT_NAME "quant_event"
    FOLDER "modules"
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    DESCRIPTION "Event bus for quant trading system"
)

# ==============================================================================
# 23. 可选基准测试
# ==============================================================================
if(QUANT_EVENT_ENABLE_BENCHMARK AND QUANT_BUILD_BENCHMARKS)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/bench_event_bus.cpp")
        add_executable(bench_event_bus
            bench_event_bus.cpp
        )
        target_link_libraries(bench_event_bus PRIVATE
            quant::event
            benchmark::benchmark
        )
        set_target_properties(bench_event_bus PROPERTIES
            FOLDER "benchmarks"
        )
    endif()
endif()

# ==============================================================================
# 24. 编译摘要
# ==============================================================================
message(STATUS "")
message(STATUS "╔══════════════════════════════════════════════════════════════╗")
message(STATUS "║  event 模块配置摘要                                           ║")
message(STATUS "╠══════════════════════════════════════════════════════════════╣")
message(STATUS "║  版本:             ${QUANT_EVENT_MODULE_VERSION}")
message(STATUS "║  必需源文件数:     3")
message(STATUS "║  可选源文件数:     ${CMAKE_MATCH_COUNT}")
message(STATUS "║  头文件数:         4")
message(STATUS "║  依赖:             quant::common")
message(STATUS "║  编译器:           ${QUANT_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "╠══════════════════════════════════════════════════════════════╣")
message(STATUS "║  Tracing:          ${QUANT_EVENT_ENABLE_TRACING}")
message(STATUS "║  Persistence:      ${QUANT_EVENT_ENABLE_PERSISTENCE}")
message(STATUS "║  FlatBuffers:      ${flatbuffers_FOUND}")
message(STATUS "║  Prometheus:       ${QUANT_EVENT_HAS_PROMETHEUS}")
message(STATUS "║  PCH:              ${QUANT_ENABLE_PCH}")
message(STATUS "╚══════════════════════════════════════════════════════════════╝")
message(STATUS "")
