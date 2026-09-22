# ==============================================================================
# 币安 BTC/ETH 3分钟量化交易系统 - indicator 模块构建配置
# ==============================================================================
# @file    src/indicator/CMakeLists.txt
# @module  indicator
# @type    core
# @name    CMakeLists
# @version 1.0.1
# @brief   技术指标计算模块构建配置
#          依赖: quant::common (assembly_order=10)
#          assembly_order: 30
#          已修复 20 类运行时问题
#
# 包含文件:
#   头文件 (6):
#     indicator.core.calculator.hpp          指标计算
#     indicator.core.band.hpp                动态带宽
#     indicator.core.support_resistance.hpp  支撑阻力
#     indicator.core.vectorized.hpp          SIMD 加速（header-only）
#     indicator.core.cache.hpp               缓存
#     indicator.core.scoring.hpp             多因子评分
#
#   源文件 (5):
#     indicator.core.calculator.cpp
#     indicator.core.band.cpp
#     indicator.core.support_resistance.cpp
#     indicator.core.cache.cpp
#     indicator.core.scoring.cpp
#
#   契约 (1):
#     indicator.core.contract.json
#
# 使用方式（下游模块）:
#   target_link_libraries(strategy PRIVATE quant::indicator)
# ==============================================================================

# ==============================================================================
# 1. 模块元信息
# ==============================================================================
set(QUANT_INDICATOR_MODULE_NAME "indicator")
set(QUANT_INDICATOR_MODULE_VERSION "1.0.1")
set(QUANT_INDICATOR_ASSEMBLY_ORDER 30)
set(QUANT_INDICATOR_CONTRACT_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/indicator.core.contract.json")

message(STATUS "配置模块: indicator (v${QUANT_INDICATOR_MODULE_VERSION}, order=${QUANT_INDICATOR_ASSEMBLY_ORDER})")

# ==============================================================================
# 2. 构建选项
# ==============================================================================
option(QUANT_INDICATOR_ENABLE_SIMD
    "启用 SIMD 优化（AVX2+FMA）" ON)
option(QUANT_INDICATOR_STRICT_SOURCES
    "源文件缺失时中断构建" ON)
option(QUANT_INDICATOR_ENABLE_WARNINGS
    "启用严格编译警告" ON)

# ==============================================================================
# 3. 依赖校验（前置检查）
# ==============================================================================

# 3.1 quant::common 必须存在
if(NOT TARGET quant::common)
    message(FATAL_ERROR
        "indicator 模块依赖 quant::common，但该目标不存在。\n"
        "  请确认 src/common/CMakeLists.txt 已被 add_subdirectory 处理。\n"
        "  indicator 的 assembly_order=30，common 为 10。")
endif()

# 3.2 编译器版本校验（依赖 common 已做，此处冗余保护）
if(QUANT_COMPILER_ID STREQUAL "GCC"
   AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 11)
    message(FATAL_ERROR "indicator 需要 GCC >= 11，当前 ${CMAKE_CXX_COMPILER_VERSION}")
endif()

# ==============================================================================
# 4. 源文件清单
# ==============================================================================
set(QUANT_INDICATOR_SOURCES
    indicator.core.calculator.cpp
    indicator.core.band.cpp
    indicator.core.support_resistance.cpp
    indicator.core.cache.cpp
    indicator.core.scoring.cpp
)

# ==============================================================================
# 5. 头文件清单（含 header-only）
# ==============================================================================
set(QUANT_INDICATOR_HEADERS
    indicator.core.calculator.hpp
    indicator.core.band.hpp
    indicator.core.support_resistance.hpp
    indicator.core.vectorized.hpp      # header-only
    indicator.core.cache.hpp
    indicator.core.scoring.hpp
)

# ==============================================================================
# 6. 编译期文件存在性校验
# ==============================================================================
foreach(_src ${QUANT_INDICATOR_SOURCES})
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_src}")
        if(QUANT_INDICATOR_STRICT_SOURCES)
            message(FATAL_ERROR
                "indicator 模块缺少源文件: ${_src}\n"
                "  期望路径: ${CMAKE_CURRENT_SOURCE_DIR}/${_src}")
        else()
            message(WARNING "indicator 模块缺少源文件: ${_src}")
        endif()
    endif()
endforeach()

foreach(_hdr ${QUANT_INDICATOR_HEADERS})
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_hdr}")
        if(QUANT_INDICATOR_STRICT_SOURCES)
            message(FATAL_ERROR
                "indicator 模块缺少头文件: ${_hdr}\n"
                "  期望路径: ${CMAKE_CURRENT_SOURCE_DIR}/${_hdr}")
        else()
            message(WARNING "indicator 模块缺少头文件: ${_hdr}")
        endif()
    endif()
endforeach()

# ==============================================================================
# 7. 库目标
# ==============================================================================
add_library(quant_indicator STATIC
    ${QUANT_INDICATOR_SOURCES}
    ${QUANT_INDICATOR_HEADERS}
)

# 命名空间别名（下游统一使用 quant::indicator）
add_library(quant::indicator ALIAS quant_indicator)

# ==============================================================================
# 8. 源文件 IDE 分组
# ==============================================================================
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    PREFIX "Sources"
    FILES
        ${QUANT_INDICATOR_SOURCES}
        ${QUANT_INDICATOR_HEADERS}
        ${QUANT_INDICATOR_CONTRACT_FILE}
)

# ==============================================================================
# 9. Include 目录
# ==============================================================================
target_include_directories(quant_indicator
    PUBLIC
        # 下游通过 #include <indicator/indicator.core.calculator.hpp> 引用
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# ==============================================================================
# 10. C++ 标准（传递到下游）
# ==============================================================================
target_compile_features(quant_indicator PUBLIC cxx_std_20)

# ==============================================================================
# 11. 链接 quant::common
# ==============================================================================
target_link_libraries(quant_indicator
    PUBLIC
        quant::common     # 传递类型、错误、时间戳、SIMD、日志
)

# 可选：若 common 提供了统一的 compiler_options 接口库
if(TARGET quant::compiler_options)
    target_link_libraries(quant_indicator PRIVATE quant::compiler_options)
endif()

# ==============================================================================
# 12. SIMD 编译选项
# ==============================================================================
# 说明：vectorized.hpp 通过 `#if defined(__AVX2__)` 检测，需要在编译时启用
#       不使用 -march=native（破坏可移植性）
if(QUANT_INDICATOR_ENABLE_SIMD)
    if(QUANT_COMPILER_ID MATCHES "GCC|Clang|AppleClang")
        # 先检测编译器是否支持
        include(CheckCXXCompilerFlag)

        check_cxx_compiler_flag("-mavx2" HAS_AVX2_FLAG)
        check_cxx_compiler_flag("-mfma"  HAS_FMA_FLAG)

        if(HAS_AVX2_FLAG)
            target_compile_options(quant_indicator PRIVATE -mavx2)
        endif()

        if(HAS_FMA_FLAG)
            target_compile_options(quant_indicator PRIVATE -mfma)
        endif()

        # NEON（ARM）
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
            target_compile_options(quant_indicator PRIVATE -mfpu=neon)
        endif()

    elseif(QUANT_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(quant_indicator PRIVATE /arch:AVX2)
    endif()
endif()

# ==============================================================================
# 13. 编译警告（仅本目标，不传播）
# ==============================================================================
if(QUANT_INDICATOR_ENABLE_WARNINGS)
    if(QUANT_COMPILER_ID MATCHES "GCC|Clang|AppleClang")
        target_compile_options(quant_indicator PRIVATE
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
        target_compile_options(quant_indicator PRIVATE
            /W4
            /permissive-
            /Zc:__cplusplus
            /Zc:preprocessor
            /utf-8
            /external:W0
        )
    endif()
endif()

# ==============================================================================
# 14. 平台特定编译定义
# ==============================================================================
if(WIN32)
    target_compile_definitions(quant_indicator PUBLIC
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
    )
endif()

# ==============================================================================
# 15. 位置无关代码
# ==============================================================================
set_target_properties(quant_indicator PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

# ==============================================================================
# 16. 预编译头（PCH）
# ==============================================================================
if(QUANT_ENABLE_PCH AND COMMAND target_precompile_headers)
    target_precompile_headers(quant_indicator PRIVATE
        <algorithm>
        <array>
        <atomic>
        <cmath>
        <cstddef>
        <cstdint>
        <limits>
        <memory>
        <mutex>
        <optional>
        <shared_mutex>
        <span>
        <string>
        <string_view>
        <vector>
    )
    message(STATUS "indicator: PCH 已启用")
endif()

# ==============================================================================
# 17. 契约文件安装（项目规范要求）
# ==============================================================================
if(EXISTS "${QUANT_INDICATOR_CONTRACT_FILE}")

    # 17.1 确保目标目录存在
    add_custom_command(TARGET quant_indicator PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${CMAKE_BINARY_DIR}/contracts"
    )

    # 17.2 复制契约
    add_custom_command(TARGET quant_indicator POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${QUANT_INDICATOR_CONTRACT_FILE}"
            "${CMAKE_BINARY_DIR}/contracts/indicator.core.contract.json"
        COMMENT "复制 indicator 契约文件"
    )

    # 17.3 计算并注入哈希
    file(SHA256 "${QUANT_INDICATOR_CONTRACT_FILE}"
         _quant_indicator_contract_hash)

    add_custom_command(TARGET quant_indicator POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DCONTRACT_FILE=${CMAKE_BINARY_DIR}/contracts/indicator.core.contract.json
            -DCONTRACT_HASH=${_quant_indicator_contract_hash}
            -P "${CMAKE_SOURCE_DIR}/cmake/scripts/inject_contract_hash.cmake"
        COMMENT "注入 indicator 契约哈希"
    )
endif()

# ==============================================================================
# 18. 安装规则
# ==============================================================================
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# 18.1 安装静态库
install(TARGETS quant_indicator
    EXPORT quant-indicator-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# 18.2 安装头文件
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/indicator
    FILES_MATCHING
        PATTERN "*.hpp"
        PATTERN "*.h"
        PATTERN "*.contract.json"
        PATTERN "CMakeLists.txt" EXCLUDE
)

# 18.3 导出目标
install(EXPORT quant-indicator-targets
    FILE quant-indicator-targets.cmake
    NAMESPACE quant::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/quant-indicator
)

# 18.4 生成配置文件
configure_package_config_file(
    "${CMAKE_SOURCE_DIR}/cmake/quant-indicator-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/quant-indicator-config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/quant-indicator
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/quant-indicator-config-version.cmake"
    VERSION ${QUANT_INDICATOR_MODULE_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/quant-indicator-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/quant-indicator-config-version.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/quant-indicator
)

# ==============================================================================
# 19. 目标属性
# ==============================================================================
set_target_properties(quant_indicator PROPERTIES
    OUTPUT_NAME "quant_indicator"
    FOLDER "modules"
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    DESCRIPTION "Technical indicators for quant trading system"
)

# Windows Debug/Release 区分
if(WIN32)
    set_target_properties(quant_indicator PROPERTIES
        DEBUG_POSTFIX "d"
    )
endif()

# ==============================================================================
# 20. 测试（可选）
# ==============================================================================
if(QUANT_BUILD_TESTS AND EXISTS "${CMAKE_SOURCE_DIR}/tests/indicator")
    add_subdirectory("${CMAKE_SOURCE_DIR}/tests/indicator"
                     "${CMAKE_BINARY_DIR}/tests/indicator")
endif()

# ==============================================================================
# 21. 基准测试（可选）
# ==============================================================================
if(QUANT_BUILD_BENCHMARKS AND EXISTS "${CMAKE_SOURCE_DIR}/benchmarks/indicator")
    add_subdirectory("${CMAKE_SOURCE_DIR}/benchmarks/indicator"
                     "${CMAKE_BINARY_DIR}/benchmarks/indicator")
endif()

# ==============================================================================
# 22. 编译摘要
# ==============================================================================
message(STATUS "")
message(STATUS "╔══════════════════════════════════════════════════════════════╗")
message(STATUS "║  indicator 模块配置摘要                                       ║")
message(STATUS "╠══════════════════════════════════════════════════════════════╣")
message(STATUS "║  版本:           ${QUANT_INDICATOR_MODULE_VERSION}")
message(STATUS "║  装配顺序:       ${QUANT_INDICATOR_ASSEMBLY_ORDER}")
message(STATUS "║  源文件数:       ${CMAKE_MATCH_COUNT}")
message(STATUS "║  头文件数:       6 (含 vectorized.hpp header-only)")
message(STATUS "║  依赖:           quant::common")
message(STATUS "╠══════════════════════════════════════════════════════════════╣")
message(STATUS "║  SIMD:           ${QUANT_INDICATOR_ENABLE_SIMD}")
message(STATUS "║  AVX2 支持:      ${HAS_AVX2_FLAG}")
message(STATUS "║  FMA 支持:       ${HAS_FMA_FLAG}")
message(STATUS "║  严格警告:       ${QUANT_INDICATOR_ENABLE_WARNINGS}")
message(STATUS "║  严格源校验:     ${QUANT_INDICATOR_STRICT_SOURCES}")
message(STATUS "║  PCH:            ${QUANT_ENABLE_PCH}")
message(STATUS "║  编译器:         ${QUANT_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "╚══════════════════════════════════════════════════════════════╝")
message(STATUS "")
