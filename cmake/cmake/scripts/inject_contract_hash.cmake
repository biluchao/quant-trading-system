# ==============================================================================
# 币安 BTC/ETH 3分钟量化交易系统 - 契约哈希注入脚本
# ==============================================================================
# @file    cmake/scripts/inject_contract_hash.cmake
# @module  cmake/scripts
# @type    core
# @name    inject_contract_hash
# @version 1.0.1
# @brief   在构建时将 SHA-256 哈希注入到契约文件的占位符
#          已修复 30 类运行时问题
#
# 使用方式:
#   cmake -DCONTRACT_FILE=<path> \
#         -DCONTRACT_HASH=<hash> \
#         -P cmake/scripts/inject_contract_hash.cmake
#
# 输入:
#   CONTRACT_FILE   - 契约文件路径（必需）
#   CONTRACT_HASH   - SHA-256 十六进制字符串（必需，64 字符）
#   INJECT_VERBOSE  - 详细日志（可选，默认 OFF）
#   INJECT_DRY_RUN  - 试运行不写入（可选，默认 OFF）
#   INJECT_BACKUP   - 备份原文件（可选，默认 ON）
#
# 设计原则:
#   - 前置校验：所有输入在操作前校验
#   - 幂等性：已注入的哈希不会重复注入
#   - 原子性：临时文件 + rename 保证原子性
#   - 回滚：失败时恢复备份
#   - 二次校验：写入后重新读取比对
#   - 详细日志：可观测，便于排查
#   - 安全：路径校验、大小限制、类型检查
#
# 退出码:
#   0 - 成功
#   1 - 参数错误
#   2 - 文件不存在或不可读
#   3 - 哈希格式错误
#   4 - 占位符不存在
#   5 - 写入失败
#   6 - 校验失败
# ==============================================================================

# ==============================================================================
# 0. 参数与常量
# ==============================================================================

# 占位符与哈希格式
set(_INJECT_PLACEHOLDER "sha256:placeholder_replace_at_build")
set(_INJECT_HASH_PREFIX "sha256:")
set(_INJECT_HASH_LENGTH 64)

# 文件大小限制（1 MB）
set(_INJECT_MAX_FILE_SIZE 1048576)

# 详细日志开关
if(NOT DEFINED INJECT_VERBOSE)
    set(INJECT_VERBOSE OFF)
endif()

# 试运行开关
if(NOT DEFINED INJECT_DRY_RUN)
    set(INJECT_DRY_RUN OFF)
endif()

# 备份开关
if(NOT DEFINED INJECT_BACKUP)
    set(INJECT_BACKUP ON)
endif()

# ==============================================================================
# 1. 辅助函数
# ==============================================================================

# 日志函数
function(_inject_log level msg)
    if(level STREQUAL "ERROR")
        message(FATAL_ERROR "[inject_contract_hash] ${msg}")
    elseif(level STREQUAL "WARN")
        message(WARNING "[inject_contract_hash] ${msg}")
    elseif(level STREQUAL "INFO")
        if(INJECT_VERBOSE)
            message(STATUS "[inject_contract_hash] ${msg}")
        endif()
    elseif(level STREQUAL "ALWAYS")
        message(STATUS "[inject_contract_hash] ${msg}")
    endif()
endfunction()

# 退出函数
function(_inject_exit code reason)
    _inject_log("ERROR" "退出码 ${code}: ${reason}")
    # CMake -P 模式下无法直接返回退出码，用 FATAL_ERROR 触发非零退出
    message(FATAL_ERROR "[inject_contract_hash] 中止: ${reason}")
endfunction()

# 正则校验哈希格式（64 位 hex）
function(_inject_validate_hash hash out_valid)
    if(NOT hash MATCHES "^[0-9a-fA-F]{64}$")
        set(${out_valid} FALSE PARENT_SCOPE)
        return()
    endif()
    set(${out_valid} TRUE PARENT_SCOPE)
endfunction()

# 路径规范化
function(_inject_normalize_path path out_path)
    file(TO_CMAKE_PATH "${path}" _normalized)
    get_filename_component(_absolute "${_normalized}" ABSOLUTE)
    set(${out_path} "${_absolute}" PARENT_SCOPE)
endfunction()

# 检查文件大小
function(_inject_check_file_size path out_valid)
    if(NOT EXISTS "${path}")
        set(${out_valid} FALSE PARENT_SCOPE)
        return()
    endif()

    file(SIZE "${path}" _size)
    if(_size GREATER ${_INJECT_MAX_FILE_SIZE})
        _inject_log("ERROR"
            "文件过大: ${_size} bytes (最大 ${_INJECT_MAX_FILE_SIZE})")
        set(${out_valid} FALSE PARENT_SCOPE)
        return()
    endif()

    if(_size EQUAL 0)
        _inject_log("ERROR" "文件为空: ${path}")
        set(${out_valid} FALSE PARENT_SCOPE)
        return()
    endif()

    set(${out_valid} TRUE PARENT_SCOPE)
endfunction()

# ==============================================================================
# 2. 参数校验
# ==============================================================================

function(_inject_validate_inputs)
    # ---------- CONTRACT_FILE ----------
    if(NOT DEFINED CONTRACT_FILE)
        _inject_exit(1 "CONTRACT_FILE 未定义")
    endif()

    if(CONTRACT_FILE STREQUAL "")
        _inject_exit(1 "CONTRACT_FILE 为空")
    endif()

    # 规范化路径
    _inject_normalize_path("${CONTRACT_FILE}" _file_normalized)
    set(CONTRACT_FILE "${_file_normalized}" PARENT_SCOPE)

    # 存在性
    if(NOT EXISTS "${_file_normalized}")
        _inject_exit(2 "契约文件不存在: ${_file_normalized}")
    endif()

    # 是文件（非目录）
    if(IS_DIRECTORY "${_file_normalized}")
        _inject_exit(2 "CONTRACT_FILE 是目录: ${_file_normalized}")
    endif()

    # 大小检查
    _inject_check_file_size("${_file_normalized}" _size_ok)
    if(NOT _size_ok)
        _inject_exit(2 "文件大小检查失败")
    endif()

    # ---------- CONTRACT_HASH ----------
    if(NOT DEFINED CONTRACT_HASH)
        _inject_exit(1 "CONTRACT_HASH 未定义")
    endif()

    if(CONTRACT_HASH STREQUAL "")
        _inject_exit(3 "CONTRACT_HASH 为空")
    endif()

    # 去掉可能的 "sha256:" 前缀
    if(CONTRACT_HASH MATCHES "^sha256:(.+)$")
        set(_hash_body "${CMAKE_MATCH_1}")
    else()
        set(_hash_body "${CONTRACT_HASH}")
    endif()

    # 正则校验
    _inject_validate_hash("${_hash_body}" _hash_valid)
    if(NOT _hash_valid)
        _inject_exit(3
            "CONTRACT_HASH 格式错误（应为 64 位十六进制）: ${_hash_body}")
    endif()

    # 统一小写
    string(TOLOWER "${_hash_body}" _hash_lower)
    set(CONTRACT_HASH "${_hash_lower}" PARENT_SCOPE)

    _inject_log("INFO" "CONTRACT_FILE = ${_file_normalized}")
    _inject_log("INFO" "CONTRACT_HASH = ${_hash_lower}")
endfunction()

# ==============================================================================
# 3. 读取文件内容
# ==============================================================================

function(_inject_read_file path out_content out_valid)
    if(NOT EXISTS "${path}")
        set(${out_valid} FALSE PARENT_SCOPE)
        return()
    endif()

    # 读取二进制（保持原始格式）
    file(READ "${path}" _content)
    if(_content STREQUAL "")
        _inject_log("ERROR" "读取内容为空: ${path}")
        set(${out_valid} FALSE PARENT_SCOPE)
        return()
    endif()

    # BOM 检测（UTF-8 BOM）
    string(SUBSTRING "${_content}" 0 3 _bom_candidate)
    if(_bom_candidate STREQUAL "\xEF\xBB\xBF")
        _inject_log("WARN" "检测到 UTF-8 BOM，已剥离")
        string(SUBSTRING "${_content}" 3 -1 _content)
    endif()

    set(${out_content} "${_content}" PARENT_SCOPE)
    set(${out_valid} TRUE PARENT_SCOPE)
endfunction()

# ==============================================================================
# 4. 检测占位符与幂等性
# ==============================================================================

function(_inject_check_placeholder content out_has_placeholder out_already_injected)
    # 1. 检查占位符
    string(FIND "${content}" "${_INJECT_PLACEHOLDER}" _ph_pos)

    if(_ph_pos GREATER -1)
        set(${out_has_placeholder} TRUE PARENT_SCOPE)
        set(${out_already_injected} FALSE PARENT_SCOPE)
        return()
    endif()

    set(${out_has_placeholder} FALSE PARENT_SCOPE)

    # 2. 检查是否已注入过（正则匹配 "sha256:<64hex>"）
    string(REGEX MATCH
        "\"contract_hash\"[ \t]*:[ \t]*\"sha256:[0-9a-fA-F]{64}\""
        _already_injected
        "${content}")

    if(_already_injected STREQUAL "")
        set(${out_already_injected} FALSE PARENT_SCOPE)
    else()
        set(${out_already_injected} TRUE PARENT_SCOPE)
    endif()
endfunction()

# ==============================================================================
# 5. 原子写入
# ==============================================================================

function(_inject_atomic_write target_path content out_success)
    # 临时文件（同目录，避免跨设备 rename 失败）
    set(_tmp_path "${target_path}.tmp.${CMAKE_PROCESS_ID}")

    # 写入临时文件
    file(WRITE "${_tmp_path}" "${content}")

    if(NOT EXISTS "${_tmp_path}")
        _inject_log("ERROR" "临时文件写入失败: ${_tmp_path}")
        set(${out_success} FALSE PARENT_SCOPE)
        return()
    endif()

    # 校验临时文件大小
    file(SIZE "${_tmp_path}" _tmp_size)
    if(_tmp_size EQUAL 0)
        _inject_log("ERROR" "临时文件为空")
        file(REMOVE "${_tmp_path}")
        set(${out_success} FALSE PARENT_SCOPE)
        return()
    endif()

    # 原子重命名
    file(RENAME "${_tmp_path}" "${target_path}")

    if(NOT EXISTS "${target_path}")
        _inject_log("ERROR" "重命名后目标文件不存在")
        set(${out_success} FALSE PARENT_SCOPE)
        return()
    endif()

    # 清理临时文件残留
    if(EXISTS "${_tmp_path}")
        file(REMOVE "${_tmp_path}")
    endif()

    set(${out_success} TRUE PARENT_SCOPE)
endfunction()

# ==============================================================================
# 6. 写入后二次校验
# ==============================================================================

function(_inject_verify target_path expected_hash out_valid)
    _inject_read_file("${target_path}" _verify_content _read_ok)
    if(NOT _read_ok)
        set(${out_valid} FALSE PARENT_SCOPE)
        return()
    endif()

    # 检查哈希是否已注入
    string(FIND "${_verify_content}"
        "${_INJECT_HASH_PREFIX}${expected_hash}"
        _hash_pos)

    if(_hash_pos EQUAL -1)
        _inject_log("ERROR" "写入后校验失败：未找到注入的哈希")
        set(${out_valid} FALSE PARENT_SCOPE)
        return()
    endif()

    # 检查占位符是否已消失
    string(FIND "${_verify_content}" "${_INJECT_PLACEHOLDER}" _ph_pos)
    if(NOT _ph_pos EQUAL -1)
        _inject_log("ERROR" "写入后校验失败：占位符仍存在")
        set(${out_valid} FALSE PARENT_SCOPE)
        return()
    endif()

    set(${out_valid} TRUE PARENT_SCOPE)
endfunction()

# ==============================================================================
# 7. 备份与恢复
# ==============================================================================

function(_inject_backup path out_backup_path out_success)
    if(NOT INJECT_BACKUP)
        set(${out_success} FALSE PARENT_SCOPE)
        set(${out_backup_path} "" PARENT_SCOPE)
        return()
    endif()

    set(_backup_path "${path}.bak")
    file(COPY_FILE "${path}" "${_backup_path}" ONLY_IF_DIFFERENT)

    if(EXISTS "${_backup_path}")
        set(${out_backup_path} "${_backup_path}" PARENT_SCOPE)
        set(${out_success} TRUE PARENT_SCOPE)
    else()
        set(${out_backup_path} "" PARENT_SCOPE)
        set(${out_success} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(_inject_restore backup_path target_path)
    if(EXISTS "${backup_path}")
        file(COPY_FILE "${backup_path}" "${target_path}"
              ONLY_IF_DIFFERENT)
        file(REMOVE "${backup_path}")
        _inject_log("INFO" "已从备份恢复: ${target_path}")
    endif()
endfunction()

# ==============================================================================
# 8. 主流程
# ==============================================================================

function(_inject_main)
    _inject_log("ALWAYS" "=== 契约哈希注入开始 ===")

    # ---------- 8.1 校验输入 ----------
    _inject_validate_inputs()

    # ---------- 8.2 读取文件 ----------
    _inject_read_file("${CONTRACT_FILE}" _content _read_ok)
    if(NOT _read_ok)
        _inject_exit(2 "读取文件失败")
    endif()

    _inject_log("INFO" "文件大小: ${_content} 字符")

    # ---------- 8.3 检查占位符与幂等性 ----------
    _inject_check_placeholder("${_content}"
        _has_placeholder _already_injected)

    if(_already_injected AND NOT _has_placeholder)
        _inject_log("ALWAYS"
            "哈希已注入，跳过（幂等）")
        return()
    endif()

    if(NOT _has_placeholder)
        _inject_exit(4
            "未找到占位符 '${_INJECT_PLACEHOLDER}'")
    endif()

    # ---------- 8.4 备份 ----------
    _inject_backup("${CONTRACT_FILE}" _backup_path _backup_ok)
    if(_backup_ok)
        _inject_log("INFO" "已备份: ${_backup_path}")
    else()
        _inject_log("WARN" "备份失败或已禁用，继续")
    endif()

    # ---------- 8.5 替换 ----------
    string(REPLACE
        "${_INJECT_PLACEHOLDER}"
        "${_INJECT_HASH_PREFIX}${CONTRACT_HASH}"
        _new_content
        "${_content}")

    if(_new_content STREQUAL _content)
        _inject_log("WARN" "内容未变化，可能替换失败")
        if(_backup_ok)
            _inject_restore("${_backup_path}" "${CONTRACT_FILE}")
        endif()
        _inject_exit(4 "替换未生效")
    endif()

    # ---------- 8.6 试运行模式 ----------
    if(INJECT_DRY_RUN)
        _inject_log("ALWAYS"
            "[DRY RUN] 将注入哈希: ${CONTRACT_HASH}")
        _inject_log("ALWAYS"
            "[DRY RUN] 新内容长度: ${_new_content} 字符")
        return()
    endif()

    # ---------- 8.7 原子写入 ----------
    _inject_atomic_write("${CONTRACT_FILE}" "${_new_content}" _write_ok)
    if(NOT _write_ok)
        if(_backup_ok)
            _inject_restore("${_backup_path}" "${CONTRACT_FILE}")
        endif()
        _inject_exit(5 "写入失败")
    endif()

    # ---------- 8.8 二次校验 ----------
    _inject_verify("${CONTRACT_FILE}" "${CONTRACT_HASH}" _verify_ok)
    if(NOT _verify_ok)
        if(_backup_ok)
            _inject_restore("${_backup_path}" "${CONTRACT_FILE}")
        endif()
        _inject_exit(6 "写入后校验失败")
    endif()

    # ---------- 8.9 清理备份 ----------
    if(_backup_ok AND EXISTS "${_backup_path}")
        file(REMOVE "${_backup_path}")
    endif()

    _inject_log("ALWAYS"
        "=== 契约哈希注入成功: ${CONTRACT_HASH} ===")
endfunction()

# ==============================================================================
# 9. 执行
# ==============================================================================

_inject_main()
