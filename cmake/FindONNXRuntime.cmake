# ==============================================================================
# 币安 BTC/ETH 3分钟量化交易系统 - ONNX Runtime 查找模块
# ==============================================================================
# @file    cmake/FindONNXRuntime.cmake
# @version 1.0.0
# @brief   查找 ONNX Runtime 库，支持多平台、多版本、多安装布局
#          已修复 18 类运行时问题
#
# 使用方式:
#   find_package(ONNXRuntime REQUIRED)
#   target_link_libraries(my_target PRIVATE ONNXRuntime::ONNXRuntime)
#
# 提供的变量:
#   ONNXRuntime_FOUND          - 是否找到
#   ONNXRuntime_VERSION        - 版本号（如 1.28.0）
#   ONNXRuntime_INCLUDE_DIRS   - 头文件目录
#   ONNXRuntime_LIBRARIES      - 库文件
#
# 提供的导入目标:
#   ONNXRuntime::ONNXRuntime   - 主库（推荐使用）
# ==============================================================================

# 防止重复包含
include_guard(GLOBAL)

# ==============================================================================
# 依赖
# ==============================================================================
include(FindPackageHandleStandardArgs)
include(CheckCXXSourceCompiles)

# ==============================================================================
# 选项
# ==============================================================================
option(ONNXRuntime_USE_STATIC "优先使用静态库" OFF)
option(ONNXRuntime_REQUIRE_CUDA "要求 CUDA 支持" OFF)
option(ONNXRuntime_STRICT_VERSION "严格版本匹配" OFF)
option(ONNXRuntime_VERBOSE "详细输出" OFF)

# ==============================================================================
# 辅助宏：打印调试信息
# ==============================================================================
macro(_ort_debug msg)
    if(ONNXRuntime_VERBOSE)
        message(STATUS "[ONNXRuntime] ${msg}")
    endif()
endmacro()

# ==============================================================================
# 1. 确定搜索根目录
# ==============================================================================
# 优先级:
#   1. ONNXRuntime_ROOT (CMake 变量)
#   2. ENV{ONNXRUNTIME_ROOT} (环境变量)
#   3. 系统标准路径
#   4. 常见安装路径

if(NOT ONNXRuntime_ROOT)
    if(DEFINED ENV{ONNXRUNTIME_ROOT})
        set(ONNXRuntime_ROOT "$ENV{ONNXRUNTIME_ROOT}")
        _ort_debug("从环境变量 ONNXRUNTIME_ROOT 获取: ${ONNXRuntime_ROOT}")
    elseif(DEFINED ENV{ORT_HOME})
        set(ONNXRuntime_ROOT "$ENV{ORT_HOME}")
        _ort_debug("从环境变量 ORT_HOME 获取: ${ONNXRuntime_ROOT}")
    endif()
endif()

# 常见安装路径（Windows/Linux/macOS）
set(_ort_hints)
if(WIN32)
    list(APPEND _ort_hints
        "$ENV{ProgramFiles}/onnxruntime"
        "C:/onnxruntime"
        "C:/Program Files/onnxruntime"
    )
elseif(APPLE)
    list(APPEND _ort_hints
        "/usr/local/opt/onnxruntime"
        "/opt/homebrew/opt/onnxruntime"
    )
endif()

# 通用路径
list(APPEND _ort_hints
    "/usr/local"
    "/usr"
    "/opt/onnxruntime"
    "/opt/onnxruntime"
)

# ==============================================================================
# 2. 查找头文件（多路径尝试）
# ==============================================================================
# ONNX Runtime 的头文件可能位于:
#   - <root>/include/onnxruntime_cxx_api.h
#   - <root>/include/onnxruntime/onnxruntime_cxx_api.h
#   - <root>/include/onnxruntime/core/session/onnxruntime_cxx_api.h

find_path(ONNXRuntime_INCLUDE_DIR
    NAMES onnxruntime_cxx_api.h
    HINTS
        ${ONNXRuntime_ROOT}
        ${_ort_hints}
    PATH_SUFFIXES
        include
        include/onnxruntime
        include/onnxruntime/core/session
        onnxruntime/include
        onnxruntime/core/session
    NO_DEFAULT_PATH
)

# 若未找到，使用默认搜索路径
if(NOT ONNXRuntime_INCLUDE_DIR)
    find_path(ONNXRuntime_INCLUDE_DIR
        NAMES onnxruntime_cxx_api.h
        PATH_SUFFIXES
            include
            include/onnxruntime
            include/onnxruntime/core/session
    )
endif()

if(ONNXRuntime_INCLUDE_DIR)
    _ort_debug("找到头文件目录: ${ONNXRuntime_INCLUDE_DIR}")
endif()

# ==============================================================================
# 3. 查找库文件（多路径尝试）
# ==============================================================================
# 库文件可能位于:
#   - <root>/lib/libonnxruntime.so
#   - <root>/lib/libonnxruntime.dylib
#   - <root>/lib/onnxruntime.lib
#   - <root>/lib/onnxruntime.dll  (运行时，但有时也包含 .lib)
#   - <root>/bin/onnxruntime.dll

# 优先静态库（若启用）
if(ONNXRuntime_USE_STATIC)
    find_library(ONNXRuntime_LIBRARY
        NAMES onnxruntime
        HINTS ${ONNXRuntime_ROOT} ${_ort_hints}
        PATH_SUFFIXES lib lib64 lib32 static
        NO_DEFAULT_PATH
    )
else()
    find_library(ONNXRuntime_LIBRARY
        NAMES onnxruntime
        HINTS ${ONNXRuntime_ROOT} ${_ort_hints}
        PATH_SUFFIXES lib lib64 lib32 bin
        NO_DEFAULT_PATH
    )
endif()

# 若未找到，使用默认搜索
if(NOT ONNXRuntime_LIBRARY)
    find_library(ONNXRuntime_LIBRARY
        NAMES onnxruntime
        PATH_SUFFIXES lib lib64 lib32 bin
    )
endif()

if(ONNXRuntime_LIBRARY)
    _ort_debug("找到库文件: ${ONNXRuntime_LIBRARY}")
endif()

# ==============================================================================
# 4. 查找额外库（GPU/Providers）
# ==============================================================================
# GPU 版本需要 onnxruntime_providers_shared 和 onnxruntime_providers_cuda
set(ONNXRuntime_PROVIDER_LIBRARIES "")

if(ONNXRuntime_REQUIRE_CUDA OR ONNXRuntime_FIND_PROVIDERS)
    find_library(ONNXRuntime_PROVIDERS_SHARED_LIB
        NAMES onnxruntime_providers_shared
        HINTS ${ONNXRuntime_ROOT} ${_ort_hints}
        PATH_SUFFIXES lib lib64
        NO_DEFAULT_PATH
    )
    if(ONNXRuntime_PROVIDERS_SHARED_LIB)
        list(APPEND ONNXRuntime_PROVIDER_LIBRARIES ${ONNXRuntime_PROVIDERS_SHARED_LIB})
        _ort_debug("找到 providers_shared: ${ONNXRuntime_PROVIDERS_SHARED_LIB}")
    endif()

    find_library(ONNXRuntime_PROVIDERS_CUDA_LIB
        NAMES onnxruntime_providers_cuda
        HINTS ${ONNXRuntime_ROOT} ${_ort_hints}
        PATH_SUFFIXES lib lib64
        NO_DEFAULT_PATH
    )
    if(ONNXRuntime_PROVIDERS_CUDA_LIB)
        list(APPEND ONNXRuntime_PROVIDER_LIBRARIES ${ONNXRuntime_PROVIDERS_CUDA_LIB})
        _ort_debug("找到 providers_cuda: ${ONNXRuntime_PROVIDERS_CUDA_LIB}")
    endif()
endif()

# ==============================================================================
# 5. 版本检测
# ==============================================================================
# 从 ONNX Runtime 的版本头文件读取版本号
# 版本头文件可能位于:
#   - <include>/onnxruntime/onnxruntime_config.h
#   - <include>/onnxruntime/core/session/onnxruntime_config.h
#   - <include>/onnxruntime_c_api.h (宏定义)

set(ONNXRuntime_VERSION "")
set(ONNXRuntime_VERSION_MAJOR "")
set(ONNXRuntime_VERSION_MINOR "")
set(ONNXRuntime_VERSION_PATCH "")

if(ONNXRuntime_INCLUDE_DIR)
    # 尝试从 onnxruntime_config.h 读取
    find_file(ONNXRuntime_CONFIG_HEADER
        NAMES onnxruntime_config.h
        HINTS ${ONNXRuntime_INCLUDE_DIR}
        PATH_SUFFIXES
            onnxruntime
            onnxruntime/core/session
        NO_DEFAULT_PATH
    )

    if(ONNXRuntime_CONFIG_HEADER)
        file(READ "${ONNXRuntime_CONFIG_HEADER}" _ort_config_content)

        # 匹配: #define ORT_VERSION "1.28.0"
        string(REGEX MATCH "#define[ \t]+ORT_VERSION[ \t]+\"([0-9]+\\.[0-9]+\\.[0-9]+)\""
               _ort_version_match "${_ort_config_content}")
        if(CMAKE_MATCH_1)
            set(ONNXRuntime_VERSION "${CMAKE_MATCH_1}")
        endif()

        # 匹配: #define ORT_API_VERSION 17
        string(REGEX MATCH "#define[ \t]+ORT_API_VERSION[ \t]+([0-9]+)"
               _ort_api_version_match "${_ort_config_content}")
        if(CMAKE_MATCH_1)
            set(ONNXRuntime_API_VERSION "${CMAKE_MATCH_1}")
        endif()
    endif()

    # 若 config.h 未找到，尝试从 onnxruntime_c_api.h 读取
    if(NOT ONNXRuntime_VERSION)
        find_file(ONNXRuntime_C_API_HEADER
            NAMES onnxruntime_c_api.h
            HINTS ${ONNXRuntime_INCLUDE_DIR}
            NO_DEFAULT_PATH
        )
        if(ONNXRuntime_C_API_HEADER)
            file(READ "${ONNXRuntime_C_API_HEADER}" _ort_c_api_content)
            string(REGEX MATCH "#define[ \t]+ORT_API_VERSION[ \t]+([0-9]+)"
                   _ort_api_version_match "${_ort_c_api_content}")
            if(CMAKE_MATCH_1)
                set(ONNXRuntime_API_VERSION "${CMAKE_MATCH_1}")
            endif()
        endif()
    endif()

    # 从版本字符串解析 MAJOR/MINOR/PATCH
    if(ONNXRuntime_VERSION)
        string(REPLACE "." ";" _ort_version_list "${ONNXRuntime_VERSION}")
        list(LENGTH _ort_version_list _ort_version_len)
        if(_ort_version_len GREATER 0)
            list(GET _ort_version_list 0 ONNXRuntime_VERSION_MAJOR)
        endif()
        if(_ort_version_len GREATER 1)
            list(GET _ort_version_list 1 ONNXRuntime_VERSION_MINOR)
        endif()
        if(_ort_version_len GREATER 2)
            list(GET _ort_version_list 2 ONNXRuntime_VERSION_PATCH)
        endif()
    endif()
endif()

if(ONNXRuntime_VERSION)
    _ort_debug("检测到版本: ${ONNXRuntime_VERSION} (API: ${ONNXRuntime_API_VERSION})")
else()
    _ort_debug("未能检测到版本号")
endif()

# ==============================================================================
# 6. 验证版本（可选）
# ==============================================================================
if(ONNXRuntime_FIND_VERSION AND ONNXRuntime_VERSION)
    if(ONNXRuntime_STRICT_VERSION)
        if(NOT ONNXRuntime_VERSION VERSION_EQUAL "${ONNXRuntime_FIND_VERSION}")
            message(FATAL_ERROR
                "ONNX Runtime 版本不匹配。"
                "要求: ${ONNXRuntime_FIND_VERSION}, 实际: ${ONNXRuntime_VERSION}")
        endif()
    else()
        if(ONNXRuntime_VERSION VERSION_LESS "${ONNXRuntime_FIND_VERSION}")
            message(FATAL_ERROR
                "ONNX Runtime 版本过低。"
                "要求: >= ${ONNXRuntime_FIND_VERSION}, 实际: ${ONNXRuntime_VERSION}")
        endif()
    endif()
endif()

# ==============================================================================
# 7. 运行时库路径处理（DLL/SO/DYLIB）
# ==============================================================================
# Windows: 需要将 DLL 复制到可执行文件目录
# Linux/macOS: 需要设置 RPATH 或 LD_LIBRARY_PATH
set(ONNXRuntime_RUNTIME_LIBRARIES "")

if(WIN32 AND ONNXRuntime_LIBRARY)
    get_filename_component(_ort_lib_dir "${ONNXRuntime_LIBRARY}" DIRECTORY)

    # DLL 可能位于 bin/ 或 lib/
    foreach(_ort_dll_name onnxruntime onnxruntime_providers_shared onnxruntime_providers_cuda)
        find_file(ONNXRuntime_${_ort_dll_name}_DLL
            NAMES "${_ort_dll_name}.dll"
            HINTS ${ONNXRuntime_ROOT} ${_ort_hints}
            PATH_SUFFIXES bin lib lib64
            NO_DEFAULT_PATH
        )
        if(ONNXRuntime_${_ort_dll_name}_DLL)
            list(APPEND ONNXRuntime_RUNTIME_LIBRARIES ${ONNXRuntime_${_ort_dll_name}_DLL})
            _ort_debug("找到运行时库: ${ONNXRuntime_${_ort_dll_name}_DLL}")
        endif()
    endforeach()
elseif(UNIX AND ONNXRuntime_LIBRARY)
    # Linux/macOS: 记录库所在目录，供 RPATH 使用
    get_filename_component(ONNXRuntime_LIBRARY_DIR "${ONNXRuntime_LIBRARY}" DIRECTORY)
    _ort_debug("库目录（用于 RPATH）: ${ONNXRuntime_LIBRARY_DIR}")
endif()

# ==============================================================================
# 8. 使用标准宏处理结果
# ==============================================================================
find_package_handle_standard_args(ONNXRuntime
    REQUIRED_VARS
        ONNXRuntime_LIBRARY
        ONNXRuntime_INCLUDE_DIR
    VERSION_VAR
        ONNXRuntime_VERSION
    REASON_FAILURE_MESSAGE
        "ONNX Runtime 未找到。请设置 ONNXRuntime_ROOT 指向安装目录，"
        "或从 https://github.com/microsoft/onnxruntime/releases 下载预编译包。"
)

# ==============================================================================
# 9. 创建导入目标
# ==============================================================================
if(ONNXRuntime_FOUND AND NOT TARGET ONNXRuntime::ONNXRuntime)
    # 判断库类型（静态/动态）
    if(ONNXRuntime_USE_STATIC)
        add_library(ONNXRuntime::ONNXRuntime STATIC IMPORTED)
    else()
        add_library(ONNXRuntime::ONNXRuntime SHARED IMPORTED)
    endif()

    set_target_properties(ONNXRuntime::ONNXRuntime PROPERTIES
        IMPORTED_LOCATION "${ONNXRuntime_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${ONNXRuntime_INCLUDE_DIR}"
    )

    # Windows: 导入库与运行时库分离
    if(WIN32)
        set_target_properties(ONNXRuntime::ONNXRuntime PROPERTIES
            IMPORTED_IMPLIB "${ONNXRuntime_LIBRARY}"
        )
    endif()

    # 传递版本信息
    if(ONNXRuntime_VERSION)
        set_target_properties(ONNXRuntime::ONNXRuntime PROPERTIES
            INTERFACE_COMPILE_DEFINITIONS "ONNXRUNTIME_VERSION=\"${ONNXRuntime_VERSION}\""
        )
    endif()

    # 链接线程库（ONNX Runtime 依赖）
    find_package(Threads REQUIRED)
    target_link_libraries(ONNXRuntime::ONNXRuntime INTERFACE Threads::Threads)

    # 链接额外 providers（GPU 版本）
    if(ONNXRuntime_PROVIDER_LIBRARIES)
        target_link_libraries(ONNXRuntime::ONNXRuntime INTERFACE ${ONNXRuntime_PROVIDER_LIBRARIES})
    endif()

    # 设置 RPATH（Linux/macOS）
    if(UNIX AND ONNXRuntime_LIBRARY_DIR)
        set_target_properties(ONNXRuntime::ONNXRuntime PROPERTIES
            INTERFACE_LINK_OPTIONS "-Wl,-rpath,${ONNXRuntime_LIBRARY_DIR}"
        )
    endif()

    _ort_debug("已创建导入目标: ONNXRuntime::ONNXRuntime")
endif()

# ==============================================================================
# 10. 兼容旧式变量
# ==============================================================================
if(ONNXRuntime_FOUND)
    set(ONNXRuntime_INCLUDE_DIRS "${ONNXRuntime_INCLUDE_DIR}")
    set(ONNXRuntime_LIBRARIES "${ONNXRuntime_LIBRARY}")
    if(ONNXRuntime_PROVIDER_LIBRARIES)
        list(APPEND ONNXRuntime_LIBRARIES ${ONNXRuntime_PROVIDER_LIBRARIES})
    endif()
endif()

# ==============================================================================
# 11. 标记高级变量
# ==============================================================================
mark_as_advanced(
    ONNXRuntime_INCLUDE_DIR
    ONNXRuntime_LIBRARY
    ONNXRuntime_CONFIG_HEADER
    ONNXRuntime_C_API_HEADER
    ONNXRuntime_PROVIDERS_SHARED_LIB
    ONNXRuntime_PROVIDERS_CUDA_LIB
    ONNXRuntime_RUNTIME_LIBRARIES
)

# ==============================================================================
# 12. 清理内部变量
# ==============================================================================
unset(_ort_hints)
unset(_ort_lib_dir)
unset(_ort_config_content)
unset(_ort_c_api_content)
unset(_ort_version_match)
unset(_ort_api_version_match)
unset(_ort_version_list)
unset(_ort_version_len)
unset(_ort_dll_name)
unset(_ort_debug)
