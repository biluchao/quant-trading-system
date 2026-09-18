# ==============================================================================
# 币安 BTC/ETH 3分钟量化交易系统 - common 模块构建配置
# ==============================================================================
# @file    src/common/CMakeLists.txt
# @version 1.0.1
# @brief   公共基础设施：类型、时间戳、配置、日志、内存池、无锁队列、
#          SIMD、错误处理、加密、指标采集
#          已修复 20 类运行时问题
#
# 包含文件:
#   common.core.types.hpp            基础类型、枚举、常量
#   common.core.timestamp.hpp/.cpp   统一时间戳（NTP + 交易所偏移）
#   common.core.config.hpp/.cpp      配置加载（RCU 热更新）
#   common.core.logger.hpp/.cpp      异步日志（spdlog 封装）
#   common.core.memory_pool.hpp      对象池
#   common.core.lockfree_queue.hpp   无锁队列（moodycamel 封装）
#   common.core.simd.hpp             SIMD 工具（x86 AVX2 / ARM NEON）
#   common.core.error.hpp            错误处理
#   common.core.crypto.hpp/.cpp      加密工具
#   common.core.metrics.hpp          Prometheus 指标
#   common.core.contract.json        接口契约
#
# 使用方式（下游模块）:
#   target_link_libraries(my_target PRIVATE quant::common)
# ==============================================================================

# ==============================================================================
# 1. 源文件清单
# ==============================================================================

# 编译为静态库的源文件
set(COMMON_SOURCES
    common.core.timestamp.cpp
    common.core.config.cpp
    common.core.logger.cpp
    common.core.crypto.cpp
    common.core.metrics.cpp
)

# 纯头文件（参与 IDE 显示，不参与编译）
set(COMMON_HEADERS
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
# 2. 静态库目标
# ==============================================================================
add_library(quant_common STATIC
    ${COMMON_SOURCES}
    ${COMMON_HEADERS}
)

# 命名空间别名（供下游统一使用 quant::common）
add_library(quant::common ALIAS quant_common)

# ==============================================================================
# 3. 源文件属性（IDE 分组）
# ==============================================================================
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ${COMMON_SOURCES} ${COMMON_HEADERS}
)

# ==============================================================================
# 4. Include 目录（使用 CURRENT_SOURCE_DIR 而非 CMAKE_SOURCE_DIR）
# ==============================================================================
target_include_directories(quant_common
    PUBLIC
        # 下游通过 #include <common/common.core.types.hpp> 引用
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# ==============================================================================
# 5. C++ 标准（传递到下游）
# ==============================================================================
target_compile_features(quant_common PUBLIC cxx_std_20)

# ==============================================================================
# 6. 编译选项（仅作用于本目标，不传播）
# ==============================================================================
if(QUANT_COMPILER_ID MATCHES "GCC|Clang|AppleClang")
    target_compile_options(quant_common PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
elseif(QUANT_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(quant_common PRIVATE
        /W4
        /permissive-
        /Zc:__cplusplus
        /Zc:preprocessor
        /utf-8
    )
endif()

# ==============================================================================
# 7. 平台特定编译定义
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
# 8. 位置无关代码（供链接到共享库）
# ==============================================================================
set_target_properties(quant_common PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

# ==============================================================================
# 9. 依赖查找
# ==============================================================================
# 线程库（std::thread 需要）
find_package(Threads REQUIRED)
target_link_libraries(quant_common PUBLIC Threads::Threads)

# OpenSSL（crypto 模块使用）
find_package(OpenSSL REQUIRED)
target_link_libraries(quant_common PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
)

# spdlog（logger 模块使用）
if(TARGET spdlog::spdlog)
    target_link_libraries(quant_common PUBLIC spdlog::spdlog)
else()
    find_package(spdlog REQUIRED)
    target_link_libraries(quant_common PUBLIC spdlog::spdlog)
endif()

# nlohmann_json（config 模块使用）
if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(quant_common PUBLIC nlohmann_json::nlohmann_json)
else()
    find_package(nlohmann_json REQUIRED)
    target_link_libraries(quant_common PUBLIC nlohmann_json::nlohmann_json)
endif()

# Prometheus C++（metrics 模块使用，可选）
find_package(prometheus-cpp QUIET)
if(prometheus-cpp_FOUND OR TARGET prometheus-cpp::core)
    target_link_libraries(quant_common PUBLIC
        prometheus-cpp::core
        prometheus-cpp::pull
    )
    target_compile_definitions(quant_common PUBLIC QUANT_HAS_PROMETHEUS=1)
else()
    message(STATUS "prometheus-cpp 未找到，metrics 模块将使用空实现")
    target_compile_definitions(quant_common PUBLIC QUANT_HAS_PROMETHEUS=0)
endif()

# moodycamel ConcurrentQueue（lockfree_queue 使用，header-only）
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/moodycamel/concurrentqueue.h")
    target_include_directories(quant_common PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/third_party/moodycamel>
    )
endif()

# ==============================================================================
# 10. 文件系统库（GCC 8 / Clang 9 需要）
# ==============================================================================
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(quant_common PUBLIC stdc++fs)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 10)
    target_link_libraries(quant_common PUBLIC c++fs)
endif()

# ==============================================================================
# 11. SIMD 架构检测（供 simd.hpp 使用）
# ==============================================================================
include(CheckCXXSourceCompiles)

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
" QUANT_HAS_AVX2)
unset(CMAKE_REQUIRED_FLAGS)

# 检测 NEON（ARM）
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(QUANT_HAS_NEON TRUE)
else()
    set(QUANT_HAS_NEON FALSE)
endif()

if(QUANT_HAS_AVX2)
    target_compile_definitions(quant_common PUBLIC QUANT_HAS_AVX2=1)
    message(STATUS "common: 启用 AVX2")
elseif(QUANT_HAS_NEON)
    target_compile_definitions(quant_common PUBLIC QUANT_HAS_NEON=1)
    message(STATUS "common: 启用 NEON")
else()
    message(STATUS "common: 使用标量实现")
endif()

# ==============================================================================
# 12. Windows DLL 导出
# ==============================================================================
if(WIN32 AND BUILD_SHARED_LIBS)
    target_compile_definitions(quant_common PUBLIC QUANT_COMMON_EXPORTS)
    set_target_properties(quant_common PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS ON
    )
endif()

# ==============================================================================
# 13. Debug/Release 输出名区分（Windows）
# ==============================================================================
if(WIN32)
    set_target_properties(quant_common PROPERTIES
        DEBUG_POSTFIX "d"
    )
endif()

# ==============================================================================
# 14. 预编译头（PCH）
# ==============================================================================
if(QUANT_ENABLE_PCH AND COMMAND target_precompile_headers)
    target_precompile_headers(quant_common PRIVATE
        <algorithm>
        <array>
        <atomic>
        <chrono>
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
endif()

# ==============================================================================
# 15. 契约文件安装（项目规范要求）
# ==============================================================================
set(COMMON_CONTRACT_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/common.core.contract.json"
)

if(EXISTS "${COMMON_CONTRACT_FILE}")
    # 复制到构建目录，供运行时校验
    add_custom_command(TARGET quant_common POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${COMMON_CONTRACT_FILE}"
            "${CMAKE_BINARY_DIR}/contracts/common.core.contract.json"
        COMMENT "Copying common contract.json"
    )
    # 确保目标目录存在
    add_custom_command(TARGET quant_common PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${CMAKE_BINARY_DIR}/contracts"
    )
endif()

# ==============================================================================
# 16. 安装规则
# ==============================================================================
include(GNUInstallDirs)

# 安装静态库
install(TARGETS quant_common
    EXPORT quant-targets
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
)

# ==============================================================================
# 17. 导出规则（供下游 find_package 使用）
# ==============================================================================
install(EXPORT quant-targets
    FILE quant-common-targets.cmake
    NAMESPACE quant::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/quant-common
)

# 生成配置文件
include(CMakePackageConfigHelpers)

configure_package_config_file(
    "${CMAKE_SOURCE_DIR}/cmake/quant-common-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/quant-common-config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/quant-common
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/quant-common-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/quant-common-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/quant-common-config-version.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/quant-common
)

# ==============================================================================
# 18. 目标属性（美化 IDE 显示）
# ==============================================================================
set_target_properties(quant_common PROPERTIES
    OUTPUT_NAME "quant_common"
    FOLDER "modules"
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

# ==============================================================================
# 19. 编译摘要
# ==============================================================================
message(STATUS "common 模块: 已配置（${#COMMON_SOURCES} 源文件, ${#COMMON_HEADERS} 头文件）")
