# ==============================================================================
# 币安 BTC/ETH 3分钟量化交易系统 - 顶层 CMakeLists.txt（生产级修复版）
# ==============================================================================
# @file    CMakeLists.txt
# @version 1.0.1
# @author  quant-team
# @brief   顶层构建配置（已修复 25 类运行时问题）
#
# 使用方法:
#   cmake -B build/release -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/release -j$(nproc)
#
# 或使用预设:
#   cmake --preset release
#   cmake --build --preset release
# ==============================================================================

# ==============================================================================
# CMake 版本要求
# ==============================================================================
cmake_minimum_required(VERSION 3.20)

# ==============================================================================
# 项目定义
# ==============================================================================
project(quant_trading_system
    VERSION 1.0.0
    DESCRIPTION "Binance BTC/ETH 3-minute quantitative trading system"
    HOMEPAGE_URL "https://github.com/your-org/quant-trading-system"
    LANGUAGES CXX
)

# ==============================================================================
# 构建类型默认值
# ==============================================================================
# 单配置生成器（Makefile/Ninja）默认 Release
# 多配置生成器（VS/Xcode）不设置，由 --config 决定
get_property(IS_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT IS_MULTI_CONFIG AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

# ==============================================================================
# 编译器检测（必须 C++20）
# ==============================================================================
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 11)
        message(FATAL_ERROR "GCC 版本必须 ≥ 11（当前 ${CMAKE_CXX_COMPILER_VERSION}）")
    endif()
    set(QUANT_COMPILER_NAME "GCC")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
        message(FATAL_ERROR "Clang 版本必须 ≥ 14（当前 ${CMAKE_CXX_COMPILER_VERSION}）")
    endif()
    set(QUANT_COMPILER_NAME "Clang")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19.30)
        message(FATAL_ERROR "MSVC 版本必须 ≥ 19.30（VS 2022）")
    endif()
    set(QUANT_COMPILER_NAME "MSVC")
else()
    message(WARNING "未测试的编译器: ${CMAKE_CXX_COMPILER_ID}")
    set(QUANT_COMPILER_NAME "Unknown")
endif()

message(STATUS "编译器: ${QUANT_COMPILER_NAME} ${CMAKE_CXX_COMPILER_VERSION}")

# ==============================================================================
# 全局 CMake 设置
# ==============================================================================

# 统一输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# 多配置生成器需要为每个配置单独指定
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    string(TOUPPER ${config} CONFIG_UPPER)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG_UPPER}
        ${CMAKE_BINARY_DIR}/bin/$<LOWER_CASE:${config}>)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${CONFIG_UPPER}
        ${CMAKE_BINARY_DIR}/lib/$<LOWER_CASE:${config}>)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${CONFIG_UPPER}
        ${CMAKE_BINARY_DIR}/lib/$<LOWER_CASE:${config}>)
endforeach()

# 位置无关代码（静态库链接到共享库需要）
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 导出编译数据库（clang-tidy 需要）
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 安装时 RPATH
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
if(APPLE)
    set(CMAKE_INSTALL_RPATH "@loader_path/../lib")
elseif(UNIX)
    set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")
endif()

# 默认安装前缀
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/install"
        CACHE PATH "安装前缀" FORCE)
endif()

# ==============================================================================
# 构建选项
# ==============================================================================

option(QUANT_BUILD_TESTS         "构建测试" ON)
option(QUANT_BUILD_BENCHMARKS    "构建基准测试" ON)
option(QUANT_BUILD_BACKEND       "构建后端服务" ON)
option(QUANT_BUILD_AI            "构建 AI 服务" ON)
option(QUANT_BUILD_FRONTEND      "构建前端（需要 Node.js）" OFF)
option(QUANT_ENABLE_LTO          "启用链接时优化" ON)
option(QUANT_ENABLE_UNITY_BUILD  "启用 Unity Build（加速 CI）" OFF)
option(QUANT_ENABLE_PCH          "启用预编译头" ON)
option(QUANT_WARNINGS_AS_ERRORS  "警告视为错误" OFF)
option(QUANT_ENABLE_ASAN         "启用 AddressSanitizer" OFF)
option(QUANT_ENABLE_UBSAN        "启用 UndefinedBehaviorSanitizer" OFF)
option(QUANT_ENABLE_TSAN         "启用 ThreadSanitizer" OFF)
option(QUANT_ENABLE_MSAN         "启用 MemorySanitizer" OFF)
option(QUANT_ENABLE_COVERAGE     "启用代码覆盖率" OFF)
option(QUANT_ENABLE_CLANG_TIDY   "启用 clang-tidy" OFF)
option(QUANT_ENABLE_CPPCHECK     "启用 cppcheck" OFF)
option(QUANT_ENABLE_IPO          "启用过程间优化" ON)
option(QUANT_NATIVE_ARCH         "使用 -march=native（不推荐生产）" OFF)

# Sanitizer 互斥检查
set(_sanitizers_enabled 0)
foreach(s ASAN UBSAN TSAN MSAN)
    if(QUANT_ENABLE_${s})
        math(EXPR _sanitizers_enabled "${_sanitizers_enabled} + 1")
    endif()
endforeach()
if(_sanitizers_enabled GREATER 1)
    message(FATAL_ERROR "Sanitizer 只能启用一个（ASan/UBSan/TSan/MSan）")
endif()

# ==============================================================================
# C++ 标准
# ==============================================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)   # 禁用 GNU 扩展（可移植性）
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ==============================================================================
# 第三方依赖（Conan 优先，回退到 CMake find_package）
# ==============================================================================

# Conan 生成的文件（若存在）
if(EXISTS "${CMAKE_BINARY_DIR}/conan_toolchain.cmake")
    include("${CMAKE_BINARY_DIR}/conan_toolchain.cmake")
    message(STATUS "使用 Conan 工具链")
endif()

# vcpkg 生成的文件（若存在）
if(EXISTS "${CMAKE_BINARY_DIR}/vcpkg_installed")
    message(STATUS "使用 vcpkg 依赖")
endif()

# 查找依赖（按需启用）
find_package(Threads REQUIRED)

# Boost.Asio（网络）
find_package(Boost 1.74 REQUIRED COMPONENTS system filesystem)
if(Boost_FOUND)
    message(STATUS "Boost: ${Boost_VERSION}")
endif()

# OpenSSL（加密）
find_package(OpenSSL 1.1.1 REQUIRED)
if(OpenSSL_FOUND)
    message(STATUS "OpenSSL: ${OPENSSL_VERSION}")
endif()

# spdlog（日志）
find_package(spdlog 1.11 REQUIRED)

# nlohmann_json（JSON）
find_package(nlohmann_json 3.11 REQUIRED)

# Eigen3（线性代数）
find_package(Eigen3 3.4 REQUIRED NO_MODULE)

# PostgreSQL（数据库）
find_package(PostgreSQL REQUIRED)

# hiredis（Redis）
find_package(hiredis QUIET)

# ONNX Runtime（AI 推理）
if(QUANT_BUILD_AI)
    find_package(ONNXRuntime QUIET)
    if(NOT ONNXRuntime_FOUND)
        message(WARNING "未找到 ONNX Runtime，AI 推理将不可用")
        set(QUANT_BUILD_AI OFF)
    endif()
endif()

# GoogleTest（测试）
if(QUANT_BUILD_TESTS)
    find_package(GTest QUIET)
    if(NOT GTest_FOUND)
        message(STATUS "未找到 GTest，将从源码构建")
        include(FetchContent)
        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG        v1.14.0
            GIT_SHALLOW    TRUE
        )
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()
endif()

# Google Benchmark
if(QUANT_BUILD_BENCHMARKS)
    find_package(benchmark QUIET)
    if(NOT benchmark_FOUND)
        message(STATUS "未找到 Google Benchmark，将从源码构建")
        include(FetchContent)
        FetchContent_Declare(
            benchmark
            GIT_REPOSITORY https://github.com/google/benchmark.git
            GIT_TAG        v1.8.3
            GIT_SHALLOW    TRUE
        )
        set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(benchmark)
    endif()
endif()

# ==============================================================================
# 编译器标志（按编译器分支）
# ==============================================================================

# 通用警告
set(QUANT_WARNINGS_COMMON "")

if(QUANT_COMPILER_NAME STREQUAL "GCC" OR QUANT_COMPILER_NAME STREQUAL "Clang")
    list(APPEND QUANT_WARNINGS_COMMON
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
        -Wnull-dereference
        -Wdouble-promotion
        -Wformat=2
        -Wimplicit-fallthrough
    )

    # GCC 特有
    if(QUANT_COMPILER_NAME STREQUAL "GCC")
        list(APPEND QUANT_WARNINGS_COMMON
            -Wmisleading-indentation
            -Wduplicated-cond
            -Wduplicated-branches
            -Wlogical-op
            -Wuseless-cast
        )
    endif()

    # Clang 特有
    if(QUANT_COMPILER_NAME STREQUAL "Clang")
        list(APPEND QUANT_WARNINGS_COMMON
            -Wno-unknown-warning-option
            -Wdocumentation
        )
    endif()

elseif(QUANT_COMPILER_NAME STREQUAL "MSVC")
    list(APPEND QUANT_WARNINGS_COMMON
        /W4
        /permissive-
        /Zc:__cplusplus
        /Zc:preprocessor
        /utf-8
    )
    # 忽略第三方头文件警告
    list(APPEND QUANT_WARNINGS_COMMON /external:W0)
endif()

# 警告视为错误
set(QUANT_WARNINGS_AS_ERRORS_FLAG "")
if(QUANT_WARNINGS_AS_ERRORS)
    if(QUANT_COMPILER_NAME STREQUAL "MSVC")
        set(QUANT_WARNINGS_AS_ERRORS_FLAG /WX)
    else()
        set(QUANT_WARNINGS_AS_ERRORS_FLAG -Werror)
    endif()
endif()

# 优化标志
set(QUANT_OPT_FLAGS "")
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    if(QUANT_COMPILER_NAME STREQUAL "MSVC")
        list(APPEND QUANT_OPT_FLAGS /O2 /Ob3 /DNDEBUG)
    else()
        list(APPEND QUANT_OPT_FLAGS -O3 -DNDEBUG -fno-math-errno -fno-trapping-math)
        if(QUANT_NATIVE_ARCH)
            list(APPEND QUANT_OPT_FLAGS -march=native)
        endif()
    endif()
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(QUANT_COMPILER_NAME STREQUAL "MSVC")
        list(APPEND QUANT_OPT_FLAGS /Od /Zi /RTC1)
    else()
        list(APPEND QUANT_OPT_FLAGS -O0 -g3 -fno-omit-frame-pointer)
    endif()
endif()

# Sanitizer 标志
set(QUANT_SANITIZER_FLAGS "")
set(QUANT_SANITIZER_LINK_FLAGS "")

if(QUANT_ENABLE_ASAN)
    if(QUANT_COMPILER_NAME STREQUAL "MSVC")
        list(APPEND QUANT_SANITIZER_FLAGS /fsanitize=address)
    else()
        list(APPEND QUANT_SANITIZER_FLAGS -fsanitize=address -fno-omit-frame-pointer)
        list(APPEND QUANT_SANITIZER_LINK_FLAGS -fsanitize=address)
    endif()
    message(STATUS "启用 AddressSanitizer")
endif()

if(QUANT_ENABLE_UBSAN)
    if(NOT QUANT_COMPILER_NAME STREQUAL "MSVC")
        list(APPEND QUANT_SANITIZER_FLAGS -fsanitize=undefined -fno-omit-frame-pointer)
        list(APPEND QUANT_SANITIZER_LINK_FLAGS -fsanitize=undefined)
        message(STATUS "启用 UndefinedBehaviorSanitizer")
    endif()
endif()

if(QUANT_ENABLE_TSAN)
    if(NOT QUANT_COMPILER_NAME STREQUAL "MSVC")
        list(APPEND QUANT_SANITIZER_FLAGS -fsanitize=thread -fno-omit-frame-pointer)
        list(APPEND QUANT_SANITIZER_LINK_FLAGS -fsanitize=thread)
        message(STATUS "启用 ThreadSanitizer")
    endif()
endif()

if(QUANT_ENABLE_MSAN)
    if(QUANT_COMPILER_NAME STREQUAL "Clang")
        list(APPEND QUANT_SANITIZER_FLAGS -fsanitize=memory -fno-omit-frame-pointer)
        list(APPEND QUANT_SANITIZER_LINK_FLAGS -fsanitize=memory)
        message(STATUS "启用 MemorySanitizer")
    endif()
endif()

# 覆盖率标志
if(QUANT_ENABLE_COVERAGE)
    if(QUANT_COMPILER_NAME STREQUAL "GCC" OR QUANT_COMPILER_NAME STREQUAL "Clang")
        list(APPEND QUANT_OPT_FLAGS --coverage -fprofile-arcs -ftest-coverage)
        list(APPEND QUANT_SANITIZER_LINK_FLAGS --coverage)
    endif()
    message(STATUS "启用代码覆盖率")
endif()

# ==============================================================================
# 项目接口库（集中管理编译选项）
# ==============================================================================
add_library(quant_project_options INTERFACE)
add_library(quant::options ALIAS quant_project_options)

target_compile_features(quant_project_options INTERFACE cxx_std_20)
target_compile_options(quant_project_options INTERFACE
    ${QUANT_WARNINGS_COMMON}
    ${QUANT_WARNINGS_AS_ERRORS_FLAG}
    ${QUANT_OPT_FLAGS}
    ${QUANT_SANITIZER_FLAGS}
)
target_link_options(quant_project_options INTERFACE
    ${QUANT_SANITIZER_LINK_FLAGS}
)
target_link_libraries(quant_project_options INTERFACE
    Threads::Threads
)

# LTO/IPO
if(QUANT_ENABLE_IPO AND NOT QUANT_ENABLE_ASAN AND NOT QUANT_ENABLE_TSAN)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _ipo_supported OUTPUT _ipo_error)
    if(_ipo_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
        message(STATUS "启用 IPO/LTO")
    else()
        message(WARNING "IPO 不支持: ${_ipo_error}")
    endif()
endif()

# Unity Build
if(QUANT_ENABLE_UNITY_BUILD)
    set(CMAKE_UNITY_BUILD ON)
    set(CMAKE_UNITY_BUILD_BATCH_SIZE 16)
    message(STATUS "启用 Unity Build")
endif()

# clang-tidy
if(QUANT_ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_EXECUTABLE NAMES clang-tidy)
    if(CLANG_TIDY_EXECUTABLE)
        set(CMAKE_CXX_CLANG_TIDY
            ${CLANG_TIDY_EXECUTABLE}
            --config-file=${CMAKE_SOURCE_DIR}/.clang-tidy
        )
        message(STATUS "启用 clang-tidy: ${CLANG_TIDY_EXECUTABLE}")
    else()
        message(WARNING "未找到 clang-tidy")
    endif()
endif()

# cppcheck
if(QUANT_ENABLE_CPPCHECK)
    find_program(CPPCHECK_EXECUTABLE NAMES cppcheck)
    if(CPPCHECK_EXECUTABLE)
        set(CMAKE_CXX_CPPCHECK
            ${CPPCHECK_EXECUTABLE}
            --enable=warning,performance,portability
            --inline-suppr
            --suppress=missingIncludeSystem
        )
        message(STATUS "启用 cppcheck")
    endif()
endif()

# ==============================================================================
# 版本信息生成
# ==============================================================================

# Git commit
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE QUANT_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT QUANT_GIT_COMMIT)
    set(QUANT_GIT_COMMIT "unknown")
endif()

# Git branch
execute_process(
    COMMAND git rev-parse --abbrev-ref HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE QUANT_GIT_BRANCH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT QUANT_GIT_BRANCH)
    set(QUANT_GIT_BRANCH "unknown")
endif()

# 构建时间（UTC）
string(TIMESTAMP QUANT_BUILD_DATE "%Y-%m-%dT%H:%M:%SZ" UTC)

# 构建目录
configure_file(
    ${CMAKE_SOURCE_DIR}/cmake/version.hpp.in
    ${CMAKE_BINARY_DIR}/generated/quant/version.hpp
    @ONLY
)

include_directories(${CMAKE_BINARY_DIR}/generated)

message(STATUS "版本: ${PROJECT_VERSION}")
message(STATUS "提交: ${QUANT_GIT_COMMIT}")
message(STATUS "分支: ${QUANT_GIT_BRANCH}")
message(STATUS "构建时间: ${QUANT_BUILD_DATE}")

# ==============================================================================
# 子目录（存在性检查）
# ==============================================================================

set(QUANT_SUBDIRS
    src/common
    src/event
    src/data
    src/indicator
    src/strategy
    src/ai
    src/oms
    src/virtual_broker
    src/experience
    src/portfolio
    src/backtest
    src/attribution
    src/fault
    src/monitoring
    src/core_main
    src/backend
)

if(QUANT_BUILD_BACKEND)
    list(APPEND QUANT_SUBDIRS src/backend)
endif()

foreach(subdir ${QUANT_SUBDIRS})
    if(EXISTS ${CMAKE_SOURCE_DIR}/${subdir}/CMakeLists.txt)
        add_subdirectory(${subdir})
    else()
        message(WARNING "子目录不存在: ${subdir}")
    endif()
endforeach()

# Bootstrap
if(EXISTS ${CMAKE_SOURCE_DIR}/bootstrap/CMakeLists.txt)
    add_subdirectory(bootstrap)
endif()

# Deploy 工具
if(EXISTS ${CMAKE_SOURCE_DIR}/deploy/CMakeLists.txt)
    add_subdirectory(deploy)
endif()

# 测试
if(QUANT_BUILD_TESTS AND EXISTS ${CMAKE_SOURCE_DIR}/tests/CMakeLists.txt)
    enable_testing()
    include(CTest)
    add_subdirectory(tests)
endif()

# 基准测试
if(QUANT_BUILD_BENCHMARKS AND EXISTS ${CMAKE_SOURCE_DIR}/benchmarks/CMakeLists.txt)
    add_subdirectory(benchmarks)
endif()

# ==============================================================================
# 安装规则
# ==============================================================================

include(GNUInstallDirs)

# 可执行文件
install(TARGETS
    quant_core
    quant_backend
    quant_bootstrap
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT Runtime
    OPTIONAL
)

# 配置文件
install(DIRECTORY ${CMAKE_SOURCE_DIR}/config/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/quant/config
    FILES_MATCHING PATTERN "*.json" PATTERN "*.yaml" PATTERN "*.yml"
    COMPONENT Config
)

# 迁移脚本
install(DIRECTORY ${CMAKE_SOURCE_DIR}/migrations/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/quant/migrations
    FILES_MATCHING PATTERN "*.sql"
    COMPONENT Migrations
)

# 脚本
install(DIRECTORY ${CMAKE_SOURCE_DIR}/scripts/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/quant/scripts
    USE_SOURCE_PERMISSIONS
    FILES_MATCHING
        PATTERN "*.sh"
        PATTERN "*.py"
        PATTERN "*.bash"
    COMPONENT Scripts
)

# systemd 服务
if(UNIX AND NOT APPLE)
    install(FILES
        ${CMAKE_SOURCE_DIR}/systemd/quant-core.service
        ${CMAKE_SOURCE_DIR}/systemd/quant-backend.service
        ${CMAKE_SOURCE_DIR}/systemd/quant-bootstrap.service
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/system
        OPTIONAL
        COMPONENT Systemd
    )
endif()

# ==============================================================================
# CPack 打包
# ==============================================================================

set(CPACK_PACKAGE_NAME "quant-trading-system")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "quant-team")
set(CPACK_PACKAGE_CONTACT "ops@example.com")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Binance BTC/ETH 3-minute quantitative trading system")
set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_HOMEPAGE_URL}")

set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${CMAKE_SOURCE_DIR}/README.md")

set(CPACK_GENERATOR "TGZ")
if(APPLE)
    list(APPEND CPACK_GENERATOR "DragNDrop")
elseif(WIN32)
    list(APPEND CPACK_GENERATOR "ZIP;NSIS")
elseif(UNIX)
    find_program(DPKG_CMD dpkg)
    if(DPKG_CMD)
        list(APPEND CPACK_GENERATOR "DEB")
    endif()
    find_program(RPMBUILD_CMD rpmbuild)
    if(RPMBUILD_CMD)
        list(APPEND CPACK_GENERATOR "RPM")
    endif()
endif()

set(CPACK_DEBIAN_PACKAGE_MAINTAINER "quant-team <ops@example.com>")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libboost-system-dev, libssl-dev, libpq-dev")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)

set(CPACK_RPM_PACKAGE_LICENSE "MIT")
set(CPACK_RPM_PACKAGE_GROUP "Development/Tools")

include(CPack)

# ==============================================================================
# 打印构建摘要
# ==============================================================================
message(STATUS "")
message(STATUS "╔══════════════════════════════════════════════════════════════╗")
message(STATUS "║  构建配置摘要                                                 ║")
message(STATUS "╠══════════════════════════════════════════════════════════════╣")
message(STATUS "║  项目:      ${PROJECT_NAME} v${PROJECT_VERSION}")
message(STATUS "║  构建类型:  ${CMAKE_BUILD_TYPE}")
message(STATUS "║  编译器:    ${QUANT_COMPILER_NAME} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "║  构建目录:  ${CMAKE_BINARY_DIR}")
message(STATUS "║  安装前缀:  ${CMAKE_INSTALL_PREFIX}")
message(STATUS "╠══════════════════════════════════════════════════════════════╣")
message(STATUS "║  选项:")
message(STATUS "║    测试:              ${QUANT_BUILD_TESTS}")
message(STATUS "║    基准:              ${QUANT_BUILD_BENCHMARKS}")
message(STATUS "║    后端:              ${QUANT_BUILD_BACKEND}")
message(STATUS "║    AI:                ${QUANT_BUILD_AI}")
message(STATUS "║    LTO/IPO:           ${QUANT_ENABLE_IPO}")
message(STATUS "║    Unity Build:       ${QUANT_ENABLE_UNITY_BUILD}")
message(STATUS "║    PCH:               ${QUANT_ENABLE_PCH}")
message(STATUS "║    警告即错误:        ${QUANT_WARNINGS_AS_ERRORS}")
message(STATUS "║    ASan:              ${QUANT_ENABLE_ASAN}")
message(STATUS "║    UBSan:             ${QUANT_ENABLE_UBSAN}")
message(STATUS "║    TSan:              ${QUANT_ENABLE_TSAN}")
message(STATUS "║    覆盖率:            ${QUANT_ENABLE_COVERAGE}")
message(STATUS "║    clang-tidy:        ${QUANT_ENABLE_CLANG_TIDY}")
message(STATUS "║    原生架构优化:      ${QUANT_NATIVE_ARCH}")
message(STATUS "╚══════════════════════════════════════════════════════════════╝")
message(STATUS "")
