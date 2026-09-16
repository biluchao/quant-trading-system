# ==============================================================================
# 币安 BTC/ETH 3分钟量化交易系统 - 编译器选项模块
# ==============================================================================
# @file    cmake/CompilerOptions.cmake
# @version 1.0.0
# @brief   集中管理编译器警告、优化、Sanitizer、LTO 等选项
#
# 使用方式（在顶层 CMakeLists.txt 中）:
#   include(cmake/CompilerOptions.cmake)
#   target_link_libraries(my_target PRIVATE quant::compiler_options)
#
# 提供的接口库:
#   quant::compiler_options  - 项目通用编译选项
#   quant::warnings          - 仅警告选项
#   quant::sanitizers        - Sanitizer 选项
# ==============================================================================

# 防止重复包含
if(DEFINED QUANT_COMPILER_OPTIONS_INCLUDED)
    return()
endif()
set(QUANT_COMPILER_OPTIONS_INCLUDED TRUE)

include_guard(GLOBAL)

# ==============================================================================
# 编译器检测
# ==============================================================================
if(NOT DEFINED QUANT_COMPILER_ID)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(QUANT_COMPILER_ID "GCC")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(QUANT_COMPILER_ID "Clang")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        set(QUANT_COMPILER_ID "MSVC")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
        set(QUANT_COMPILER_ID "AppleClang")
    else()
        set(QUANT_COMPILER_ID "Unknown")
        message(WARNING "未识别的编译器: ${CMAKE_CXX_COMPILER_ID}")
    endif()
endif()

# 是否为类 GCC 编译器（GCC/Clang/AppleClang）
if(QUANT_COMPILER_ID MATCHES "^(GCC|Clang|AppleClang)$")
    set(QUANT_IS_GNU_LIKE TRUE)
else()
    set(QUANT_IS_GNU_LIKE FALSE)
endif()

message(STATUS "编译器: ${QUANT_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

# ==============================================================================
# 编译器版本检查
# ==============================================================================
if(QUANT_COMPILER_ID STREQUAL "GCC")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 11)
        message(FATAL_ERROR "GCC >= 11 必需，当前 ${CMAKE_CXX_COMPILER_VERSION}")
    endif()
elseif(QUANT_COMPILER_ID STREQUAL "Clang")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
        message(FATAL_ERROR "Clang >= 14 必需，当前 ${CMAKE_CXX_COMPILER_VERSION}")
    endif()
elseif(QUANT_COMPILER_ID STREQUAL "AppleClang")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
        message(FATAL_ERROR "AppleClang >= 14 必需，当前 ${CMAKE_CXX_COMPILER_VERSION}")
    endif()
elseif(QUANT_COMPILER_ID STREQUAL "MSVC")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19.30)
        message(FATAL_ERROR "MSVC >= 19.30 必需，当前 ${CMAKE_CXX_COMPILER_VERSION}")
    endif()
endif()

# ==============================================================================
# 选项定义
# ==============================================================================
option(QUANT_WARNINGS_AS_ERRORS  "警告视为错误" OFF)
option(QUANT_ENABLE_ASAN         "启用 AddressSanitizer" OFF)
option(QUANT_ENABLE_UBSAN        "启用 UndefinedBehaviorSanitizer" OFF)
option(QUANT_ENABLE_TSAN         "启用 ThreadSanitizer" OFF)
option(QUANT_ENABLE_MSAN         "启用 MemorySanitizer" OFF)
option(QUANT_ENABLE_COVERAGE     "启用代码覆盖率" OFF)
option(QUANT_ENABLE_IPO          "启用过程间优化（LTO）" ON)
option(QUANT_NATIVE_ARCH         "启用 -march=native（不推荐生产）" OFF)
option(QUANT_STRICT_WARNINGS     "启用严格警告集" ON)
option(QUANT_ENABLE_HARDENING    "启用安全加固编译选项" ON)

# ==============================================================================
# Sanitizer 互斥检查
# ==============================================================================
set(_quant_sanitizer_count 0)
foreach(_san ASAN UBSAN TSAN MSAN)
    if(QUANT_ENABLE_${_san})
        math(EXPR _quant_sanitizer_count "${_quant_sanitizer_count} + 1")
    endif()
endforeach()

if(_quant_sanitizer_count GREATER 1)
    message(FATAL_ERROR
        "Sanitizer 只能启用一个（ASan/UBSan/TSan/MSan）。"
        "当前启用: ${_quant_sanitizer_count} 个")
endif()

# MSan 仅 Clang 支持
if(QUANT_ENABLE_MSAN AND NOT QUANT_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "MemorySanitizer 仅 Clang 支持")
endif()

# TSan 与 ASan 不兼容（已在上面互斥）
# TSan 与覆盖率不兼容
if(QUANT_ENABLE_TSAN AND QUANT_ENABLE_COVERAGE)
    message(FATAL_ERROR "ThreadSanitizer 与覆盖率不兼容")
endif()

# ==============================================================================
# 警告选项
# ==============================================================================
set(QUANT_WARNING_FLAGS "")

if(QUANT_IS_GNU_LIKE)
    list(APPEND QUANT_WARNING_FLAGS
        -Wall
        -Wextra
        -Wpedantic
    )

    if(QUANT_STRICT_WARNINGS)
        list(APPEND QUANT_WARNING_FLAGS
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
            -Wunused-result
            -Wundef
            -Wcast-qual
            -Wpointer-arith
            -Wwrite-strings
            -Wmissing-declarations
            -Wredundant-decls
        )
    endif()

    # GCC 特有
    if(QUANT_COMPILER_ID STREQUAL "GCC")
        list(APPEND QUANT_WARNING_FLAGS
            -Wmisleading-indentation
            -Wduplicated-cond
            -Wduplicated-branches
            -Wlogical-op
            -Wuseless-cast
            -Wno-maybe-uninitialized
        )
    endif()

    # Clang/AppleClang 特有
    if(QUANT_COMPILER_ID MATCHES "^(Clang|AppleClang)$")
        list(APPEND QUANT_WARNING_FLAGS
            -Wno-unknown-warning-option
            -Wdocumentation
            -Wno-c++98-compat
            -Wno-c++98-compat-pedantic
        )
    endif()

elseif(QUANT_COMPILER_ID STREQUAL "MSVC")
    list(APPEND QUANT_WARNING_FLAGS
        /W4
        /permissive-
        /Zc:__cplusplus
        /Zc:preprocessor
        /utf-8
        /external:W0
    )

    if(QUANT_STRICT_WARNINGS)
        list(APPEND QUANT_WARNING_FLAGS
            /w14640
        )
    endif()
endif()

# 警告视为错误
if(QUANT_WARNINGS_AS_ERRORS)
    if(QUANT_COMPILER_ID STREQUAL "MSVC")
        list(APPEND QUANT_WARNING_FLAGS /WX)
    else()
        list(APPEND QUANT_WARNING_FLAGS -Werror)
    endif()
endif()

# ==============================================================================
# 优化选项
# ==============================================================================
set(QUANT_OPTIMIZATION_FLAGS "")

# 根据 CMAKE_BUILD_TYPE 设置
if(CMAKE_BUILD_TYPE)
    string(TOUPPER "${CMAKE_BUILD_TYPE}" _bt_upper)
else()
    set(_bt_upper "RELEASE")
endif()

if(_bt_upper STREQUAL "RELEASE" OR _bt_upper STREQUAL "RELWITHDEBINFO")
    if(QUANT_COMPILER_ID STREQUAL "MSVC")
        list(APPEND QUANT_OPTIMIZATION_FLAGS
            /O2
            /Ob3
            /DNDEBUG
            /GL
        )
    else()
        list(APPEND QUANT_OPTIMIZATION_FLAGS
            -O3
            -DNDEBUG
            -fno-math-errno
            -fno-trapping-math
        )
        if(QUANT_NATIVE_ARCH)
            list(APPEND QUANT_OPTIMIZATION_FLAGS -march=native)
            message(WARNING "-march=native 已启用，二进制不可移植")
        endif()
    endif()

elseif(_bt_upper STREQUAL "DEBUG")
    if(QUANT_COMPILER_ID STREQUAL "MSVC")
        list(APPEND QUANT_OPTIMIZATION_FLAGS
            /Od
            /Zi
            /RTC1
        )
    else()
        list(APPEND QUANT_OPTIMIZATION_FLAGS
            -O0
            -g3
            -fno-omit-frame-pointer
        )
    endif()

elseif(_bt_upper STREQUAL "MINSIZEREL")
    if(QUANT_COMPILER_ID STREQUAL "MSVC")
        list(APPEND QUANT_OPTIMIZATION_FLAGS /O1 /DNDEBUG)
    else()
        list(APPEND QUANT_OPTIMIZATION_FLAGS -Os -DNDEBUG)
    endif()
endif()

# ==============================================================================
# Sanitizer 选项
# ==============================================================================
set(QUANT_SANITIZER_COMPILE_FLAGS "")
set(QUANT_SANITIZER_LINK_FLAGS "")

if(QUANT_ENABLE_ASAN)
    if(QUANT_COMPILER_ID STREQUAL "MSVC")
        list(APPEND QUANT_SANITIZER_COMPILE_FLAGS /fsanitize=address)
    else()
        list(APPEND QUANT_SANITIZER_COMPILE_FLAGS
            -fsanitize=address
            -fno-omit-frame-pointer
            -fno-common
        )
        list(APPEND QUANT_SANITIZER_LINK_FLAGS -fsanitize=address)
    endif()
    message(STATUS "启用 AddressSanitizer")
endif()

if(QUANT_ENABLE_UBSAN)
    if(NOT QUANT_COMPILER_ID STREQUAL "MSVC")
        list(APPEND QUANT_SANITIZER_COMPILE_FLAGS
            -fsanitize=undefined
            -fno-omit-frame-pointer
            -fno-sanitize-recover=all
        )
        list(APPEND QUANT_SANITIZER_LINK_FLAGS -fsanitize=undefined)
        message(STATUS "启用 UndefinedBehaviorSanitizer")
    endif()
endif()

if(QUANT_ENABLE_TSAN)
    if(NOT QUANT_COMPILER_ID STREQUAL "MSVC")
        list(APPEND QUANT_SANITIZER_COMPILE_FLAGS
            -fsanitize=thread
            -fno-omit-frame-pointer
        )
        list(APPEND QUANT_SANITIZER_LINK_FLAGS -fsanitize=thread)
        message(STATUS "启用 ThreadSanitizer")
    endif()
endif()

if(QUANT_ENABLE_MSAN)
    list(APPEND QUANT_SANITIZER_COMPILE_FLAGS
        -fsanitize=memory
        -fno-omit-frame-pointer
        -fsanitize-memory-track-origins=2
    )
    list(APPEND QUANT_SANITIZER_LINK_FLAGS -fsanitize=memory)
    message(STATUS "启用 MemorySanitizer")
endif()

# ==============================================================================
# 覆盖率选项
# ==============================================================================
if(QUANT_ENABLE_COVERAGE)
    if(QUANT_IS_GNU_LIKE)
        list(APPEND QUANT_OPTIMIZATION_FLAGS
            --coverage
            -fprofile-arcs
            -ftest-coverage
        )
        list(APPEND QUANT_SANITIZER_LINK_FLAGS --coverage)
        message(STATUS "启用代码覆盖率")
    else()
        message(WARNING "覆盖率仅 GCC/Clang 支持")
    endif()
endif()

# ==============================================================================
# 安全加固选项
# ==============================================================================
set(QUANT_HARDENING_FLAGS "")

if(QUANT_ENABLE_HARDENING AND QUANT_IS_GNU_LIKE)
    # 编译期加固
    list(APPEND QUANT_HARDENING_FLAGS
        -fstack-protector-strong
        -fPIE
        -D_FORTIFY_SOURCE=2
    )

    # 链接期加固
    list(APPEND QUANT_SANITIZER_LINK_FLAGS
        -Wl,-z,relro
        -Wl,-z,now
        -Wl,-z,noexecstack
        -pie
    )

    message(STATUS "启用安全加固选项")
endif()

# ==============================================================================
# 其他通用选项
# ==============================================================================
set(QUANT_COMMON_FLAGS "")

if(QUANT_IS_GNU_LIKE)
    list(APPEND QUANT_COMMON_FLAGS
        -fno-omit-frame-pointer
    )

    # 仅在 Release 下启用
    if(_bt_upper STREQUAL "RELEASE" OR _bt_upper STREQUAL "RELWITHDEBINFO")
        list(APPEND QUANT_COMMON_FLAGS
            -fno-plt
            -fmerge-all-constants
        )
    endif()

    # 检测并启用 -march=x86-64-v3（若未启用 native）
    if(NOT QUANT_NATIVE_ARCH)
        include(CheckCXXCompilerFlag)
        check_cxx_compiler_flag("-march=x86-64-v3" QUANT_HAS_X86_64_V3)
        if(QUANT_HAS_X86_64_V3 AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
            list(APPEND QUANT_COMMON_FLAGS -march=x86-64-v3)
            message(STATUS "启用 -march=x86-64-v3")
        endif()
    endif()
endif()

# ==============================================================================
# 创建接口库
# ==============================================================================

# ---------- 通用编译选项 ----------
add_library(quant_compiler_options INTERFACE)
add_library(quant::compiler_options ALIAS quant_compiler_options)

target_compile_features(quant_compiler_options INTERFACE cxx_std_20)

target_compile_options(quant_compiler_options INTERFACE
    ${QUANT_WARNING_FLAGS}
    ${QUANT_OPTIMIZATION_FLAGS}
    ${QUANT_SANITIZER_COMPILE_FLAGS}
    ${QUANT_HARDENING_FLAGS}
    ${QUANT_COMMON_FLAGS}
)

target_link_options(quant_compiler_options INTERFACE
    ${QUANT_SANITIZER_LINK_FLAGS}
)

# ---------- 仅警告 ----------
add_library(quant_warnings INTERFACE)
add_library(quant::warnings ALIAS quant_warnings)
target_compile_options(quant_warnings INTERFACE ${QUANT_WARNING_FLAGS})

# ---------- 仅 Sanitizer ----------
add_library(quant_sanitizers INTERFACE)
add_library(quant::sanitizers ALIAS quant_sanitizers)
target_compile_options(quant_sanitizers INTERFACE ${QUANT_SANITIZER_COMPILE_FLAGS})
target_link_options(quant_sanitizers INTERFACE ${QUANT_SANITIZER_LINK_FLAGS})

# ==============================================================================
# IPO/LTO 配置
# ==============================================================================
if(QUANT_ENABLE_IPO AND _quant_sanitizer_count EQUAL 0 AND NOT QUANT_ENABLE_COVERAGE)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _quant_ipo_supported OUTPUT _quant_ipo_error)

    if(_quant_ipo_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
        message(STATUS "启用 IPO/LTO")
    else()
        message(WARNING "IPO 不支持: ${_quant_ipo_error}")
    endif()
elseif(QUANT_ENABLE_IPO)
    message(STATUS "IPO 已禁用（与 Sanitizer/覆盖率冲突）")
endif()

# ==============================================================================
# 打印配置摘要
# ==============================================================================
message(STATUS "")
message(STATUS "╔═══════════════════════════════════════════════════════════╗")
message(STATUS "║  编译器选项配置                                            ║")
message(STATUS "╠═══════════════════════════════════════════════════════════╣")
message(STATUS "║  编译器:          ${QUANT_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "║  构建类型:        ${CMAKE_BUILD_TYPE}")
message(STATUS "║  严格警告:        ${QUANT_STRICT_WARNINGS}")
message(STATUS "║  警告即错误:      ${QUANT_WARNINGS_AS_ERRORS}")
message(STATUS "║  安全加固:        ${QUANT_ENABLE_HARDENING}")
message(STATUS "║  ASan:            ${QUANT_ENABLE_ASAN}")
message(STATUS "║  UBSan:           ${QUANT_ENABLE_UBSAN}")
message(STATUS "║  TSan:            ${QUANT_ENABLE_TSAN}")
message(STATUS "║  MSan:            ${QUANT_ENABLE_MSAN}")
message(STATUS "║  覆盖率:          ${QUANT_ENABLE_COVERAGE}")
message(STATUS "║  LTO/IPO:         ${QUANT_ENABLE_IPO}")
message(STATUS "║  Native 优化:     ${QUANT_NATIVE_ARCH}")
message(STATUS "╚═══════════════════════════════════════════════════════════╝")
message(STATUS "")

# 清理内部变量
unset(_quant_sanitizer_count)
unset(_bt_upper)
unset(_san)
unset(_quant_ipo_supported)
unset(_quant_ipo_error)
