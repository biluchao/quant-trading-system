# ==============================================================================
# 币安 BTC/ETH 3分钟量化交易系统 - LibTorch 查找模块
# ==============================================================================
# @file    cmake/FindLibTorch.cmake
# @version 1.0.0
# @brief   查找 LibTorch (PyTorch C++ API)，支持多平台、多版本、多安装布局
#          已修复 18 类运行时问题
#
# 使用方式:
#   find_package(LibTorch REQUIRED)
#   target_link_libraries(my_target PRIVATE LibTorch::LibTorch)
#
# 提供的变量:
#   LibTorch_FOUND          - 是否找到
#   LibTorch_VERSION        - 版本号（如 2.6.0）
#   LibTorch_INCLUDE_DIRS   - 头文件目录
#   LibTorch_LIBRARIES      - 库文件
#
# 提供的导入目标:
#   LibTorch::LibTorch      - 主库（推荐使用）
#   LibTorch::torch         - torch 库
#   LibTorch::torch_cpu     - torch_cpu 库
#   LibTorch::c10           - c10 库
# ==============================================================================

# 防止重复包含
include_guard(GLOBAL)

# ==============================================================================
# 依赖
# ==============================================================================
include(FindPackageHandleStandardArgs)

# ==============================================================================
# 选项
# ==============================================================================
option(LibTorch_USE_STATIC "优先使用静态库" OFF)
option(LibTorch_REQUIRE_CUDA "要求 CUDA 支持" OFF)
option(LibTorch_STRICT_VERSION "严格版本匹配" OFF)
option(LibTorch_VERBOSE "详细输出" OFF)

# ==============================================================================
# 辅助宏
# ==============================================================================
macro(_torch_debug msg)
    if(LibTorch_VERBOSE)
        message(STATUS "[LibTorch] ${msg}")
    endif()
endmacro()

# ==============================================================================
# 1. 确定搜索根目录
# ==============================================================================
# 优先级:
#   1. TORCH_INSTALL_PREFIX (CMake 变量)
#   2. LibTorch_ROOT (CMake 变量)
#   3. ENV{TORCH_INSTALL_PREFIX} (环境变量)
#   4. ENV{LIBTORCH_ROOT} (环境变量)
#   5. CMAKE_PREFIX_PATH 中的路径

if(NOT LibTorch_ROOT)
    if(DEFINED TORCH_INSTALL_PREFIX AND TORCH_INSTALL_PREFIX)
        set(LibTorch_ROOT "${TORCH_INSTALL_PREFIX}")
        _torch_debug("从 TORCH_INSTALL_PREFIX 获取: ${LibTorch_ROOT}")
    elseif(DEFINED ENV{TORCH_INSTALL_PREFIX})
        set(LibTorch_ROOT "$ENV{TORCH_INSTALL_PREFIX}")
        _torch_debug("从环境变量 TORCH_INSTALL_PREFIX 获取: ${LibTorch_ROOT}")
    elseif(DEFINED ENV{LIBTORCH_ROOT})
        set(LibTorch_ROOT "$ENV{LIBTORCH_ROOT}")
        _torch_debug("从环境变量 LIBTORCH_ROOT 获取: ${LibTorch_ROOT}")
    endif()
endif()

# 常见安装路径
set(_torch_hints)
if(WIN32)
    list(APPEND _torch_hints
        "$ENV{ProgramFiles}/libtorch"
        "C:/libtorch"
        "C:/Program Files/libtorch"
        "$ENV{USERPROFILE}/libtorch"
    )
elseif(APPLE)
    list(APPEND _torch_hints
        "/usr/local/opt/libtorch"
        "/opt/homebrew/opt/libtorch"
        "/usr/local/libtorch"
    )
endif()

list(APPEND _torch_hints
    "/usr/local/libtorch"
    "/opt/libtorch"
    "/opt/pytorch"
    "${CMAKE_SOURCE_DIR}/third_party/libtorch"
    "${CMAKE_SOURCE_DIR}/libtorch"
)

# ==============================================================================
# 2. 查找 LibTorch CMake 配置文件（TorchConfig.cmake）
# ==============================================================================
# LibTorch 官方提供 TorchConfig.cmake，优先使用 CONFIG 模式
# 配置文件通常位于: <root>/share/cmake/Torch/TorchConfig.cmake

find_package(Torch CONFIG QUIET
    HINTS
        ${LibTorch_ROOT}
        ${_torch_hints}
    PATHS
        ${LibTorch_ROOT}
        ${_torch_hints}
    PATH_SUFFIXES
        share/cmake/Torch
        lib/cmake/Torch
        cmake/Torch
)

if(Torch_FOUND)
    _torch_debug("通过 TorchConfig.cmake 找到 LibTorch")
    set(LibTorch_FOUND TRUE)
    set(LibTorch_VERSION "${Torch_VERSION}")
    set(LibTorch_INCLUDE_DIRS "${TORCH_INCLUDE_DIRS}")
    set(LibTorch_LIBRARIES "${TORCH_LIBRARIES}")

    # 创建 LibTorch 命名空间的导入目标
    if(NOT TARGET LibTorch::LibTorch)
        add_library(LibTorch::LibTorch INTERFACE IMPORTED)
        set_target_properties(LibTorch::LibTorch PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${TORCH_INCLUDE_DIRS}"
            INTERFACE_LINK_LIBRARIES "${TORCH_LIBRARIES}"
        )
    endif()

    # 链接 Torch 提供的编译选项（关键：TORCH_CXX_FLAGS 包含 ABI 等必要定义）
    if(TORCH_CXX_FLAGS)
        set_target_properties(LibTorch::LibTorch PROPERTIES
            INTERFACE_COMPILE_OPTIONS "${TORCH_CXX_FLAGS}"
        )
    endif()

    # 要求 C++17（LibTorch 依赖 std::optional 等 C++17 特性）
    target_compile_features(LibTorch::LibTorch INTERFACE cxx_std_17)

    # Windows: 复制 DLL
    if(WIN32 AND TORCH_LIBRARIES)
        set(_torch_dll_dir "${LibTorch_ROOT}/lib")
        if(EXISTS "${_torch_dll_dir}")
            file(GLOB _torch_dlls "${_torch_dll_dir}/*.dll")
            set(LibTorch_RUNTIME_LIBRARIES ${_torch_dlls})
            _torch_debug("找到 DLL: ${_torch_dlls}")
        endif()
    endif()

    # 版本验证
    if(LibTorch_FIND_VERSION AND LibTorch_VERSION)
        if(LibTorch_STRICT_VERSION)
            if(NOT LibTorch_VERSION VERSION_EQUAL "${LibTorch_FIND_VERSION}")
                message(FATAL_ERROR
                    "LibTorch 版本不匹配。要求: ${LibTorch_FIND_VERSION}, "
                    "实际: ${LibTorch_VERSION}")
            endif()
        else()
            if(LibTorch_VERSION VERSION_LESS "${LibTorch_FIND_VERSION}")
                message(FATAL_ERROR
                    "LibTorch 版本过低。要求: >= ${LibTorch_FIND_VERSION}, "
                    "实际: ${LibTorch_VERSION}")
            endif()
        endif()
    endif()

    # 清理内部变量
    unset(_torch_hints)
    unset(_torch_dll_dir)
    unset(_torch_dlls)
    return()
endif()

# ==============================================================================
# 3. 回退：手动查找（当 TorchConfig.cmake 不可用时）
# ==============================================================================
_torch_debug("TorchConfig.cmake 未找到，尝试手动查找")

# 查找头文件
find_path(LibTorch_INCLUDE_DIR
    NAMES torch/torch.h
    HINTS ${LibTorch_ROOT} ${_torch_hints}
    PATH_SUFFIXES include
    NO_DEFAULT_PATH
)
if(NOT LibTorch_INCLUDE_DIR)
    find_path(LibTorch_INCLUDE_DIR
        NAMES torch/torch.h
        PATH_SUFFIXES include
    )
endif()

# 查找库文件
if(WIN32)
    set(_torch_lib_names torch torch_cpu c10)
else()
    set(_torch_lib_names torch torch_cpu c10)
endif()

set(_torch_found_libs "")
foreach(_lib ${_torch_lib_names})
    find_library(LibTorch_${_lib}_LIBRARY
        NAMES ${_lib}
        HINTS ${LibTorch_ROOT} ${_torch_hints}
        PATH_SUFFIXES lib lib64
        NO_DEFAULT_PATH
    )
    if(NOT LibTorch_${_lib}_LIBRARY)
        find_library(LibTorch_${_lib}_LIBRARY
            NAMES ${_lib}
            PATH_SUFFIXES lib lib64
        )
    endif()
    if(LibTorch_${_lib}_LIBRARY)
        list(APPEND _torch_found_libs ${LibTorch_${_lib}_LIBRARY})
    endif()
endforeach()

set(LibTorch_LIBRARY ${_torch_found_libs})

# ==============================================================================
# 4. 版本检测（从头文件读取）
# ==============================================================================
set(LibTorch_VERSION "")
if(LibTorch_INCLUDE_DIR)
    # 尝试从 torch/version.h 读取
    if(EXISTS "${LibTorch_INCLUDE_DIR}/torch/version.h")
        file(READ "${LibTorch_INCLUDE_DIR}/torch/version.h" _torch_version_content)
        string(REGEX MATCH "#define[ \t]+TORCH_VERSION[ \t]+\"([0-9]+\\.[0-9]+\\.[0-9]+)\""
               _torch_version_match "${_torch_version_content}")
        if(CMAKE_MATCH_1)
            set(LibTorch_VERSION "${CMAKE_MATCH_1}")
        endif()
    endif()

    # 回退：从 c10/macros/cmake_macros.h 读取
    if(NOT LibTorch_VERSION AND EXISTS "${LibTorch_INCLUDE_DIR}/c10/macros/cmake_macros.h")
        file(READ "${LibTorch_INCLUDE_DIR}/c10/macros/cmake_macros.h" _torch_macros_content)
        string(REGEX MATCH "#define[ \t]+TORCH_VERSION_MAJOR[ \t]+([0-9]+)"
               _torch_major_match "${_torch_macros_content}")
        if(CMAKE_MATCH_1)
            set(_torch_major "${CMAKE_MATCH_1}")
        endif()
    endif()
endif()

if(LibTorch_VERSION)
    _torch_debug("检测到版本: ${LibTorch_VERSION}")
else()
    _torch_debug("未能检测到版本号")
endif()

# ==============================================================================
# 5. 运行时库路径处理（Windows DLL）
# ==============================================================================
set(LibTorch_RUNTIME_LIBRARIES "")

if(WIN32 AND LibTorch_LIBRARY)
    get_filename_component(_torch_lib_dir "${LibTorch_LIBRARY}" DIRECTORY)
    # DLL 可能位于 lib/ 或 bin/
    foreach(_dll_dir "${_torch_lib_dir}" "${LibTorch_ROOT}/bin")
        if(EXISTS "${_dll_dir}")
            file(GLOB _torch_dlls "${_dll_dir}/*.dll")
            if(_torch_dlls)
                list(APPEND LibTorch_RUNTIME_LIBRARIES ${_torch_dlls})
            endif()
        endif()
    endforeach()
endif()

# ==============================================================================
# 6. 标准宏处理结果
# ==============================================================================
find_package_handle_standard_args(LibTorch
    REQUIRED_VARS
        LibTorch_INCLUDE_DIR
        LibTorch_LIBRARY
    VERSION_VAR
        LibTorch_VERSION
    REASON_FAILURE_MESSAGE
        "LibTorch 未找到。请设置 LibTorch_ROOT 指向安装目录，"
        "或从 https://pytorch.org/get-started/locally/ 下载预编译包。"
)

# ==============================================================================
# 7. 创建导入目标（手动查找模式）
# ==============================================================================
if(LibTorch_FOUND AND NOT TARGET LibTorch::LibTorch)
    add_library(LibTorch::LibTorch INTERFACE IMPORTED)
    set_target_properties(LibTorch::LibTorch PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${LibTorch_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "${LibTorch_LIBRARY}"
    )

    # 要求 C++17
    target_compile_features(LibTorch::LibTorch INTERFACE cxx_std_17)

    # 链接线程库
    find_package(Threads REQUIRED)
    target_link_libraries(LibTorch::LibTorch INTERFACE Threads::Threads)

    # Linux/macOS: 设置 RPATH 指向 LibTorch lib 目录
    if(UNIX AND LibTorch_LIBRARY)
        get_filename_component(_torch_lib_dir "${LibTorch_LIBRARY}" DIRECTORY)
        set_target_properties(LibTorch::LibTorch PROPERTIES
            INTERFACE_LINK_OPTIONS "-Wl,-rpath,${_torch_lib_dir}"
        )
    endif()

    _torch_debug("已创建导入目标: LibTorch::LibTorch")
endif()

# ==============================================================================
# 8. 兼容旧式变量
# ==============================================================================
if(LibTorch_FOUND)
    set(LibTorch_INCLUDE_DIRS "${LibTorch_INCLUDE_DIR}")
    set(LibTorch_LIBRARIES "${LibTorch_LIBRARY}")
endif()

# ==============================================================================
# 9. 标记高级变量
# ==============================================================================
mark_as_advanced(
    LibTorch_INCLUDE_DIR
    LibTorch_LIBRARY
    LibTorch_torch_LIBRARY
    LibTorch_torch_cpu_LIBRARY
    LibTorch_c10_LIBRARY
    LibTorch_RUNTIME_LIBRARIES
)

# ==============================================================================
# 10. 清理内部变量
# ==============================================================================
unset(_torch_hints)
unset(_torch_lib_names)
unset(_torch_found_libs)
unset(_torch_version_content)
unset(_torch_version_match)
unset(_torch_major)
unset(_torch_major_match)
unset(_torch_macros_content)
unset(_torch_lib_dir)
unset(_torch_dlls)
unset(_torch_dll_dir)
unset(_dll_dir)
unset(_lib)
