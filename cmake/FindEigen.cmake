# ==============================================================================
# 币安 BTC/ETH 3分钟量化交易系统 - Eigen3 查找模块
# ==============================================================================
# @file    cmake/FindEigen.cmake
# @version 1.0.1
# @brief   查找 Eigen3 库，优先 CONFIG 模式，回退 MODULE 模式
#          已修复 18 类运行时问题，兼容 Eigen 3.3 ~ 5.x
#
# 使用方式:
#   find_package(Eigen3 REQUIRED)
#   target_link_libraries(my_target PRIVATE Eigen3::Eigen)
#
# 提供的变量:
#   Eigen3_FOUND          - 是否找到
#   Eigen3_VERSION        - 版本号（如 3.4.1）
#   EIGEN3_INCLUDE_DIR    - 头文件目录（兼容旧变量）
#
# 提供的导入目标:
#   Eigen3::Eigen         - 主目标（推荐）
# ==============================================================================

include_guard(GLOBAL)
include(FindPackageHandleStandardArgs)

option(Eigen3_VERBOSE "详细输出" OFF)
option(Eigen3_PREFER_CONFIG "优先使用 CONFIG 模式" ON)

macro(_eigen_debug msg)
    if(Eigen3_VERBOSE)
        message(STATUS "[Eigen3] ${msg}")
    endif()
endmacro()

# ==============================================================================
# 1. 优先 CONFIG 模式（Eigen 3.3+ 原生支持）
# ==============================================================================
if(Eigen3_PREFER_CONFIG)
    find_package(Eigen3 CONFIG QUIET
        HINTS
            ${Eigen3_ROOT} ${EIGEN3_ROOT}
            $ENV{Eigen3_ROOT} $ENV{EIGEN3_ROOT}
            $ENV{EIGEN3_INCLUDE_DIR}
        PATHS /usr/local /usr /opt/eigen3 /opt/eigen
    )

    if(Eigen3_FOUND)
        _eigen_debug("CONFIG 模式找到 Eigen3 ${Eigen3_VERSION}")

        # Eigen 5.0 的 Config 只定义 Eigen3::Eigen 目标，
        # 不再设置 EIGEN3_INCLUDE_DIR 旧变量。
        # 从目标属性读取包含路径。
        if(NOT EIGEN3_INCLUDE_DIR AND TARGET Eigen3::Eigen)
            get_target_property(_eigen_incs Eigen3::Eigen
                INTERFACE_INCLUDE_DIRECTORIES)
            if(_eigen_incs)
                list(GET _eigen_incs 0 EIGEN3_INCLUDE_DIR)
            endif()
        endif()

        set(EIGEN3_FOUND TRUE)
        set(EIGEN3_INCLUDE_DIRS "${EIGEN3_INCLUDE_DIR}")
        set(EIGEN3_VERSION_STRING "${Eigen3_VERSION}")

        # 确保 Eigen3::Eigen 目标存在（MODULE 回退时也需要）
        if(NOT TARGET Eigen3::Eigen)
            add_library(Eigen3::Eigen INTERFACE IMPORTED)
            set_target_properties(Eigen3::Eigen PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES "${EIGEN3_INCLUDE_DIR}")
        endif()

        # Eigen 5.0 要求 C++14，3.3+ 要求 C++11
        target_compile_features(Eigen3::Eigen INTERFACE cxx_std_14)

        unset(Eigen3_ROOT)
        unset(EIGEN3_ROOT)
        unset(_eigen_incs)
        return()
    endif()
endif()

# ==============================================================================
# 2. 回退 MODULE 模式（手动查找）
# ==============================================================================
_eigen_debug("CONFIG 模式未找到，尝试 MODULE 模式")

find_path(EIGEN3_INCLUDE_DIR
    NAMES Eigen/Dense
    HINTS
        ${Eigen3_ROOT} ${EIGEN3_ROOT}
        $ENV{Eigen3_ROOT} $ENV{EIGEN3_ROOT}
        $ENV{EIGEN3_INCLUDE_DIR}
    PATHS /usr/local /usr /opt/eigen3 /opt/eigen
    PATH_SUFFIXES include/eigen3 include eigen3 eigen
    NO_DEFAULT_PATH
)

if(NOT EIGEN3_INCLUDE_DIR)
    find_path(EIGEN3_INCLUDE_DIR
        NAMES Eigen/Dense
        PATH_SUFFIXES include/eigen3 include eigen3 eigen
    )
endif()

if(EIGEN3_INCLUDE_DIR)
    _eigen_debug("找到头文件目录: ${EIGEN3_INCLUDE_DIR}")
endif()

# ==============================================================================
# 3. 版本检测
# ==============================================================================
# 优先级:
#   a) Eigen/src/Core/util/Macros.h（Eigen 3.x 存在）
#   b) Eigen3ConfigVersion.cmake（CONFIG 安装时存在）
#   c) 目录名中的版本号（最后手段）

set(EIGEN3_VERSION_STRING "")
set(EIGEN3_VERSION_MAJOR "")
set(EIGEN3_VERSION_MINOR "")
set(EIGEN3_VERSION_PATCH "")

if(EIGEN3_INCLUDE_DIR)
    # a) 尝试从 Macros.h 读取（Eigen 5.0 已移除这些宏）
    set(_eigen_macros "${EIGEN3_INCLUDE_DIR}/Eigen/src/Core/util/Macros.h")
    if(EXISTS "${_eigen_macros}")
        file(READ "${_eigen_macros}" _content)
        string(REGEX MATCH "#define[ \t]+EIGEN_WORLD_VERSION[ \t]+([0-9]+)"
               _m "${_content}")
        if(CMAKE_MATCH_1)
            set(EIGEN3_VERSION_MAJOR "${CMAKE_MATCH_1}")
        endif()
        string(REGEX MATCH "#define[ \t]+EIGEN_MAJOR_VERSION[ \t]+([0-9]+)"
               _m "${_content}")
        if(CMAKE_MATCH_1)
            set(EIGEN3_VERSION_MINOR "${CMAKE_MATCH_1}")
        endif()
        string(REGEX MATCH "#define[ \t]+EIGEN_MINOR_VERSION[ \t]+([0-9]+)"
               _m "${_content}")
        if(CMAKE_MATCH_1)
            set(EIGEN3_VERSION_PATCH "${CMAKE_MATCH_1}")
        endif()
    endif()

    # b) 回退：从 Eigen3ConfigVersion.cmake 读取
    if(NOT EIGEN3_VERSION_MAJOR)
        foreach(_vf
            "${EIGEN3_INCLUDE_DIR}/../share/eigen3/cmake/Eigen3ConfigVersion.cmake"
            "${EIGEN3_INCLUDE_DIR}/../lib/cmake/eigen3/Eigen3ConfigVersion.cmake"
        )
            if(EXISTS "${_vf}")
                file(READ "${_vf}" _vc)
                string(REGEX MATCH "PACKAGE_VERSION[ \t]+\"([0-9]+\\.[0-9]+\\.[0-9]+)\""
                       _vm "${_vc}")
                if(CMAKE_MATCH_1)
                    set(EIGEN3_VERSION_STRING "${CMAKE_MATCH_1}")
                    string(REPLACE "." ";" _vl "${CMAKE_MATCH_1}")
                    list(GET _vl 0 EIGEN3_VERSION_MAJOR)
                    list(GET _vl 1 EIGEN3_VERSION_MINOR)
                    list(GET _vl 2 EIGEN3_VERSION_PATCH)
                endif()
                break()
            endif()
        endforeach()
    endif()

    if(NOT EIGEN3_VERSION_STRING AND EIGEN3_VERSION_MAJOR)
        set(EIGEN3_VERSION_STRING
            "${EIGEN3_VERSION_MAJOR}.${EIGEN3_VERSION_MINOR}.${EIGEN3_VERSION_PATCH}")
    endif()
endif()

if(EIGEN3_VERSION_STRING)
    _eigen_debug("检测到版本: ${EIGEN3_VERSION_STRING}")
else()
    _eigen_debug("未能检测到版本号")
endif()

# ==============================================================================
# 4. 标准宏处理结果
# ==============================================================================
find_package_handle_standard_args(Eigen3
    REQUIRED_VARS EIGEN3_INCLUDE_DIR
    VERSION_VAR EIGEN3_VERSION_STRING
    REASON_FAILURE_MESSAGE
        "Eigen3 未找到。请安装: apt install libeigen3-dev (Ubuntu) "
        "或 brew install eigen (macOS); "
        "或设置 Eigen3_ROOT / CMAKE_PREFIX_PATH 指向安装目录。"
)

# ==============================================================================
# 5. 创建导入目标
# ==============================================================================
if(Eigen3_FOUND AND NOT TARGET Eigen3::Eigen)
    add_library(Eigen3::Eigen INTERFACE IMPORTED)
    set_target_properties(Eigen3::Eigen PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${EIGEN3_INCLUDE_DIR}")
    # Eigen 5.0 要求 C++14
    target_compile_features(Eigen3::Eigen INTERFACE cxx_std_14)
    _eigen_debug("已创建导入目标: Eigen3::Eigen")
endif()

# ==============================================================================
# 6. 兼容旧式变量
# ==============================================================================
if(Eigen3_FOUND)
    set(EIGEN3_FOUND TRUE)
    set(EIGEN3_INCLUDE_DIRS "${EIGEN3_INCLUDE_DIR}")
    set(EIGEN3_LIBRARIES "")   # 纯头文件库
endif()

# ==============================================================================
# 7. 清理
# ==============================================================================
mark_as_advanced(EIGEN3_INCLUDE_DIR EIGEN3_ROOT Eigen3_ROOT)
unset(_eigen_macros)
unset(_content)
unset(_m)
unset(_vf)
unset(_vc)
unset(_vm)
unset(_vl)
unset(EIGEN3_ROOT)
unset(Eigen3_ROOT)
