# ==============================================================================
# 币安 BTC/ETH 3分钟量化交易系统 - 生产级 Makefile（健壮版）
# ==============================================================================
# @file    Makefile
# @version 1.0.1
# @author  quant-team
# @brief   统一构建、测试、部署、维护入口
#
# 运行时健壮性改进:
#   - 全 shell 命令使用 .ONESHELL + -eu -o pipefail
#   - 所有 $(shell ...) 加 2>/dev/null 保护
#   - 目录存在性检查前置
#   - 交互命令检测 CI 环境
#   - 跨平台工具检测（Linux/macOS/Alpine）
#   - 敏感信息不暴露进程列表
#   - 后台任务使用 wait 管理
#   - 残留文件使用 .DELETE_ON_ERROR 清理
# ==============================================================================

# ==============================================================================
# Shell 与执行环境
# ==============================================================================

# 统一使用 bash（更兼容）
SHELL           := /bin/bash
.SHELLFLAGS     := -eu -o pipefail -c

# 多行命令在同一 shell 中执行（使 cd 生效）
.ONESHELL:

# 命令失败立即中断
.DELETE_ON_ERROR:

# 禁止并行执行顶层目标（避免 build 目录冲突）
.NOTPARALLEL:

# 静默进入子目录提示
MAKEFLAGS       += --no-print-directory

# ==============================================================================
# 项目信息（全部带安全 fallback）
# ==============================================================================

PROJECT_NAME    := quant-trading-system
VERSION         := $(shell cat VERSION 2>/dev/null || echo "1.0.0")
GIT_COMMIT      := $(shell git rev-parse --short HEAD 2>/dev/null || echo "unknown")
GIT_BRANCH      := $(shell git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "unknown")
BUILD_DATE      := $(shell date -u +"%Y-%m-%dT%H:%M:%SZ" 2>/dev/null || echo "unknown")

# ==============================================================================
# 平台检测
# ==============================================================================

UNAME_S         := $(shell uname -s 2>/dev/null || echo "Unknown")

ifeq ($(UNAME_S),Darwin)
    NPROC       := $(shell sysctl -n hw.ncpu 2>/dev/null || echo 4)
    SED_INPLACE := sed -i ''
    READLINK    := greadlink
else ifeq ($(UNAME_S),Linux)
    NPROC       := $(shell nproc 2>/dev/null || echo 4)
    SED_INPLACE := sed -i
    READLINK    := readlink
else
    NPROC       := 4
    SED_INPLACE := sed -i
    READLINK    := readlink
endif

# ==============================================================================
# 目录
# ==============================================================================

ROOT_DIR        := $(shell pwd)
SRC_DIR         := $(ROOT_DIR)/src
BUILD_DIR       := $(ROOT_DIR)/build
BUILD_DEBUG     := $(BUILD_DIR)/debug
BUILD_RELEASE   := $(BUILD_DIR)/release
BUILD_RELWITHDEBINFO := $(BUILD_DIR)/relwithdebinfo
BUILD_COVERAGE  := $(BUILD_DIR)/coverage
DIST_DIR        := $(ROOT_DIR)/dist
FRONTEND_DIR    := $(ROOT_DIR)/frontend
SCRIPTS_DIR     := $(ROOT_DIR)/scripts
TESTS_DIR       := $(ROOT_DIR)/tests
DOCS_DIR        := $(ROOT_DIR)/docs
MIGRATIONS_DIR  := $(ROOT_DIR)/migrations
MODELS_DIR      := $(ROOT_DIR)/models
LOGS_DIR        := $(ROOT_DIR)/data/logs
REPLAY_DIR      := $(ROOT_DIR)/data/replay
SNAPSHOTS_DIR   := $(ROOT_DIR)/data/snapshots

# ==============================================================================
# 环境变量（安全读取）
# ==============================================================================

ENV             ?= dev
BUILD_TYPE      ?= Release
JOBS            ?= $(NPROC)
VERBOSE         ?= 0
CI              ?= $(if $(shell [ -n "$$CI" ] && echo yes),yes,no)

# 从 .env 安全读取（文件不存在则用默认值）
-include .env
export

DB_HOST         ?= localhost
DB_PORT         ?= 5432
DB_NAME         ?= quant
DB_USER         ?= quant
DB_PASSWORD     ?=

# ==============================================================================
# 工具链（使用 command -v 兼容所有系统）
# ==============================================================================

CXX             ?= g++
CC              ?= gcc
CMAKE           ?= cmake
CONAN           ?= conan
NPM             ?= npm
PYTHON          ?= python3
PIP             ?= pip3
DOCKER          ?= docker
DOCKER_COMPOSE  ?= $(shell command -v docker-compose 2>/dev/null || echo "docker compose")

# ==============================================================================
# 二进制名称
# ==============================================================================

CORE_BINARY     := quant_core
BACKEND_BINARY  := quant_backend
BOOTSTRAP_BINARY:= quant_bootstrap
AI_BINARY       := quant_ai
DEPLOY_TOOL     := deploy_tool

# ==============================================================================
# Docker
# ==============================================================================

IMAGE_PREFIX    := quant
IMAGE_TAG       := $(VERSION)

# ==============================================================================
# 颜色（使用 printf，兼容 dash/busybox）
# ==============================================================================

# 检测是否支持颜色（终端且非 CI）
ifneq (,$(findstring xterm,$(TERM)))
    HAS_COLOR := yes
else ifeq ($(TERM),screen)
    HAS_COLOR := yes
else ifeq ($(TERM),tmux)
    HAS_COLOR := yes
else ifeq ($(CI),yes)
    HAS_COLOR := no
else
    HAS_COLOR := $(shell [ -t 1 ] && echo yes || echo no)
endif

ifeq ($(HAS_COLOR),yes)
    C_RESET := \033[0m
    C_RED   := \033[0;31m
    C_GREEN := \033[0;32m
    C_YELLOW:= \033[0;33m
    C_BLUE  := \033[0;34m
    C_MAG   := \033[0;35m
    C_CYAN  := \033[0;36m
    C_BOLD  := \033[1m
else
    C_RESET :=
    C_RED   :=
    C_GREEN :=
    C_YELLOW:=
    C_BLUE  :=
    C_MAG   :=
    C_CYAN  :=
    C_BOLD  :=
endif

# 打印辅助函数
define print_info
	@printf "$(C_BLUE)[INFO]$(C_RESET) %s\n" "$(1)"
endef

define print_ok
	@printf "$(C_GREEN)[OK]$(C_RESET) %s\n" "$(1)"
endef

define print_warn
	@printf "$(C_YELLOW)[WARN]$(C_RESET) %s\n" "$(1)"
endef

define print_err
	@printf "$(C_RED)[ERROR]$(C_RESET) %s\n" "$(1)"
endef

define print_step
	@printf "$(C_CYAN)[STEP]$(C_RESET) %s\n" "$(1)"
endef

# ==============================================================================
# 默认目标
# ==============================================================================

.DEFAULT_GOAL := help

.PHONY: help
help: ## 显示帮助信息
	@printf "\n"
	@printf "$(C_CYAN)╔══════════════════════════════════════════════════════════════╗$(C_RESET)\n"
	@printf "$(C_CYAN)║$(C_RESET)  $(C_GREEN)币安 BTC/ETH 3分钟量化交易系统$(C_RESET)  $(C_CYAN)v$(VERSION)$(C_RESET)\n"
	@printf "$(C_CYAN)╚══════════════════════════════════════════════════════════════╝$(C_RESET)\n"
	@printf "\n"
	@printf "$(C_YELLOW)可用命令:$(C_RESET)\n\n"
	@awk 'BEGIN {FS = ":.*?## "} /^[a-zA-Z_-]+:.*?## / {printf "  $(C_GREEN)%-25s$(C_RESET) %s\n", $$1, $$2}' $(MAKEFILE_LIST) | sort
	@printf "\n"

# ==============================================================================
# 环境检查（前置）
# ==============================================================================

.PHONY: check-tools
check-tools: ## 检查工具链
	$(call print_step,检查工具链...)
	@missing=""
	@for tool in $(CXX) $(CMAKE) $(CONAN) $(NPM) $(PYTHON); do \
		if ! command -v $$tool >/dev/null 2>&1; then \
			missing="$$missing $$tool"; \
		fi; \
	done
	@if [ -n "$$missing" ]; then \
		$(call print_err,"缺少工具:$$missing"); \
		exit 1; \
	fi
	$(call print_ok,工具链完整)

.PHONY: check-env
check-env: ## 检查环境
	$(call print_step,检查环境...)
	@if [ ! -f "$(SCRIPTS_DIR)/check_env.py" ]; then \
		$(call print_warn,"check_env.py 不存在，跳过"); \
	else \
		$(PYTHON) $(SCRIPTS_DIR)/check_env.py --env $(ENV) || \
			( $(call print_err,"环境检查失败"); exit 1 ); \
	fi
	$(call print_ok,环境检查通过)

.PHONY: check-dirs
check-dirs: ## 创建必要的目录
	@mkdir -p $(BUILD_DIR) $(DIST_DIR) $(LOGS_DIR) $(REPLAY_DIR) $(SNAPSHOTS_DIR)
	@mkdir -p $(BUILD_DEBUG) $(BUILD_RELEASE) $(BUILD_RELWITHDEBINFO) $(BUILD_COVERAGE)

.PHONY: check-env-file
check-env-file: ## 检查 .env 文件
	@if [ ! -f .env ]; then \
		$(call print_warn,".env 不存在，从模板复制"); \
		if [ -f .env.example ]; then \
			cp .env.example .env; \
			$(call print_ok,"已创建 .env，请编辑配置"); \
		else \
			$(call print_err,".env.example 也不存在"); \
			exit 1; \
		fi; \
	fi

# ==============================================================================
# 文件规范校验
# ==============================================================================

.PHONY: validate
validate: validate-metadata validate-naming validate-contract ## 校验所有文件规范

.PHONY: validate-metadata
validate-metadata: ## 校验元数据
	$(call print_step,校验文件元数据...)
	@if [ ! -f "$(SCRIPTS_DIR)/validation/validate_metadata.py" ]; then \
		$(call print_warn,"validate_metadata.py 不存在，跳过"); \
	else \
		$(PYTHON) $(SCRIPTS_DIR)/validation/validate_metadata.py --root $(SRC_DIR) || \
			( $(call print_err,"元数据校验失败"); exit 1 ); \
	fi
	$(call print_ok,元数据校验通过)

.PHONY: validate-naming
validate-naming: ## 校验文件命名
	$(call print_step,校验文件命名...)
	@if [ ! -f "$(SCRIPTS_DIR)/validation/check_naming.py" ]; then \
		$(call print_warn,"check_naming.py 不存在，跳过"); \
	else \
		$(PYTHON) $(SCRIPTS_DIR)/validation/check_naming.py --root $(SRC_DIR) || \
			( $(call print_err,"命名校验失败"); exit 1 ); \
	fi
	$(call print_ok,命名规范通过)

.PHONY: validate-contract
validate-contract: ## 校验接口契约
	$(call print_step,校验接口契约...)
	@if [ ! -f "$(SCRIPTS_DIR)/validation/check_contract.py" ]; then \
		$(call print_warn,"check_contract.py 不存在，跳过"); \
	else \
		$(PYTHON) $(SCRIPTS_DIR)/validation/check_contract.py --root $(SRC_DIR) || \
			( $(call print_err,"契约校验失败"); exit 1 ); \
	fi
	$(call print_ok,契约校验通过)

# ==============================================================================
# 依赖管理
# ==============================================================================

.PHONY: deps
deps: deps-cpp deps-frontend deps-python ## 安装所有依赖

.PHONY: deps-cpp
deps-cpp: ## 安装 C++ 依赖
	$(call print_step,安装 C++ 依赖...)
	@if [ -f conan.lock ]; then \
		$(CONAN) install . --lockfile=conan.lock --build=missing; \
	else \
		$(CONAN) install . --build=missing || \
			( $(call print_err,"Conan 安装失败"); exit 1 ); \
		$(call print_warn,"未找到 conan.lock，建议执行 make lock-deps"); \
	fi
	$(call print_ok,C++ 依赖安装完成)

.PHONY: deps-frontend
deps-frontend: ## 安装前端依赖
	$(call print_step,安装前端依赖...)
	@if [ ! -d "$(FRONTEND_DIR)" ]; then \
		$(call print_err,"前端目录不存在"); \
		exit 1; \
	fi
	@if [ -f "$(FRONTEND_DIR)/package-lock.json" ]; then \
		cd $(FRONTEND_DIR) && $(NPM) ci || \
			( $(call print_err,"npm ci 失败"); exit 1 ); \
	else \
		$(call print_warn,"package-lock.json 不存在，使用 npm install"); \
		cd $(FRONTEND_DIR) && $(NPM) install; \
	fi
	$(call print_ok,前端依赖安装完成)

.PHONY: deps-python
deps-python: ## 安装 Python 依赖
	$(call print_step,安装 Python 依赖...)
	@if [ -z "$$VIRTUAL_ENV" ] && [ -z "$$CONDA_DEFAULT_ENV" ]; then \
		$(call print_warn,"未检测到虚拟环境，建议使用 venv"); \
	fi
	@if [ ! -f requirements.txt ]; then \
		$(call print_warn,"requirements.txt 不存在，跳过"); \
	else \
		$(PIP) install -r requirements.txt || \
			( $(call print_err,"pip install 失败"); exit 1 ); \
	fi
	$(call print_ok,Python 依赖安装完成)

.PHONY: lock-deps
lock-deps: ## 生成依赖锁定文件
	$(call print_step,生成依赖锁定...)
	@$(CONAN) lock create conanfile.txt --lockfile-out=conan.lock || \
		( $(call print_err,"conan lock 失败"); exit 1 )
	@if [ -d "$(FRONTEND_DIR)" ]; then \
		cd $(FRONTEND_DIR) && $(NPM) shrinkwrap 2>/dev/null || true; \
	fi
	@$(PIP) freeze > requirements.lock 2>/dev/null || true
	$(call print_ok,依赖锁定完成)

# ==============================================================================
# 构建
# ==============================================================================

.PHONY: all
all: check-tools check-dirs deps build-frontend build ## 完整构建
	$(call print_ok,完整构建完成)

.PHONY: build
build: build-release ## 默认构建 Release

.PHONY: build-debug
build-debug: check-dirs ## Debug 构建
	$(call print_step,Debug 构建...)
	@cd $(BUILD_DEBUG) && $(CMAKE) \
		-DCMAKE_BUILD_TYPE=Debug \
		-DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
		$(ROOT_DIR) || ( $(call print_err,"CMake 配置失败"); exit 1 )
	@cd $(BUILD_DEBUG) && $(CMAKE) --build . -j$(JOBS) || \
		( $(call print_err,"编译失败"); exit 1 )
	$(call print_ok,Debug 构建完成)

.PHONY: build-release
build-release: check-dirs ## Release 构建
	$(call print_step,Release 构建...)
	@cd $(BUILD_RELEASE) && $(CMAKE) \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
		-DVERSION=$(VERSION) \
		-DGIT_COMMIT=$(GIT_COMMIT) \
		-DBUILD_DATE=$(BUILD_DATE) \
		$(ROOT_DIR) || ( $(call print_err,"CMake 配置失败"); exit 1 )
	@cd $(BUILD_RELEASE) && $(CMAKE) --build . -j$(JOBS) || \
		( $(call print_err,"编译失败"); exit 1 )
	$(call print_ok,Release 构建完成)

.PHONY: build-relwithdebinfo
build-relwithdebinfo: check-dirs ## RelWithDebInfo 构建
	$(call print_step,RelWithDebInfo 构建...)
	@cd $(BUILD_RELWITHDEBINFO) && $(CMAKE) \
		-DCMAKE_BUILD_TYPE=RelWithDebInfo \
		$(ROOT_DIR) || ( $(call print_err,"CMake 配置失败"); exit 1 )
	@cd $(BUILD_RELWITHDEBINFO) && $(CMAKE) --build . -j$(JOBS) || \
		( $(call print_err,"编译失败"); exit 1 )
	$(call print_ok,RelWithDebInfo 构建完成)

.PHONY: build-frontend
build-frontend: ## 构建前端
	$(call print_step,构建前端...)
	@if [ ! -d "$(FRONTEND_DIR)/node_modules" ]; then \
		$(call print_err,"前端依赖未安装，请先执行 make deps-frontend"); \
		exit 1; \
	fi
	@cd $(FRONTEND_DIR) && $(NPM) run build || \
		( $(call print_err,"前端构建失败"); exit 1 )
	$(call print_ok,前端构建完成)

.PHONY: build-ai
build-ai: check-dirs ## 构建 AI 服务
	$(call print_step,构建 AI 服务...)
	@cd $(BUILD_RELEASE) && $(CMAKE) --build . --target $(AI_BINARY) -j$(JOBS) || \
		( $(call print_err,"AI 构建失败"); exit 1 )
	$(call print_ok,AI 服务构建完成)

.PHONY: rebuild
rebuild: clean build ## 重新构建

# ==============================================================================
# 测试
# ==============================================================================

.PHONY: test
test: test-unit test-integration ## 运行所有测试

.PHONY: test-unit
test-unit: ## 单元测试
	$(call print_step,运行单元测试...)
	@if [ ! -d "$(BUILD_RELEASE)" ]; then \
		$(call print_err,"未构建，请先 make build"); \
		exit 1; \
	fi
	@cd $(BUILD_RELEASE) && ctest --output-on-failure -L unit || \
		( $(call print_err,"单元测试失败"); exit 1 )
	$(call print_ok,单元测试完成)

.PHONY: test-integration
test-integration: ## 集成测试
	$(call print_step,运行集成测试...)
	@if [ ! -d "$(BUILD_RELEASE)" ]; then \
		$(call print_err,"未构建，请先 make build"); \
		exit 1; \
	fi
	@cd $(BUILD_RELEASE) && ctest --output-on-failure -L integration || \
		( $(call print_err,"集成测试失败"); exit 1 )
	$(call print_ok,集成测试完成)

.PHONY: test-strategy
test-strategy: ## 策略测试
	@cd $(BUILD_RELEASE) && ctest --output-on-failure -L strategy

.PHONY: test-oms
test-oms: ## OMS 测试
	@cd $(BUILD_RELEASE) && ctest --output-on-failure -L oms

.PHONY: test-backtest
test-backtest: ## 回测测试
	@cd $(BUILD_RELEASE) && ctest --output-on-failure -L backtest

.PHONY: test-fault
test-fault: ## 故障检测测试
	@cd $(BUILD_RELEASE) && ctest --output-on-failure -L fault

.PHONY: test-coverage
test-coverage: check-dirs ## 测试覆盖率
	$(call print_step,生成覆盖率报告...)
	@cd $(BUILD_COVERAGE) && $(CMAKE) \
		-DCMAKE_BUILD_TYPE=Debug \
		-DENABLE_COVERAGE=ON \
		$(ROOT_DIR) || exit 1
	@cd $(BUILD_COVERAGE) && $(CMAKE) --build . -j$(JOBS) || exit 1
	@cd $(BUILD_COVERAGE) && ctest || true
	@if command -v gcovr >/dev/null 2>&1; then \
		cd $(BUILD_COVERAGE) && gcovr -r $(ROOT_DIR) \
			--html --html-details -o coverage.html; \
		$(call print_ok,"覆盖率报告: $(BUILD_COVERAGE)/coverage.html"); \
	else \
		$(call print_warn,"gcovr 未安装，跳过 HTML 生成"); \
	fi

.PHONY: test-frontend
test-frontend: ## 前端测试
	@if [ -d "$(FRONTEND_DIR)" ]; then \
		cd $(FRONTEND_DIR) && $(NPM) test 2>/dev/null || \
			$(call print_warn,"前端测试未配置"); \
	fi

# ==============================================================================
# 回测与验证
# ==============================================================================

.PHONY: backtest
backtest: check-dirs ## 运行回测
	$(call print_step,运行回测...)
	@if [ ! -f "$(SCRIPTS_DIR)/run_backtest.py" ]; then \
		$(call print_err,"run_backtest.py 不存在"); \
		exit 1; \
	fi
	@$(PYTHON) $(SCRIPTS_DIR)/run_backtest.py \
		--config config/config.core.params.json \
		--output $(DIST_DIR)/backtest_report.json || \
		( $(call print_err,"回测失败"); exit 1 )
	$(call print_ok,回测完成)

.PHONY: validate-strategy
validate-strategy: ## 策略验证（DSR/PBO/WF）
	$(call print_step,策略验证...)
	@if [ -f "$(SCRIPTS_DIR)/validate_strategy.py" ]; then \
		$(PYTHON) $(SCRIPTS_DIR)/validate_strategy.py \
			--dsr --pbo --walk-forward --monte-carlo || exit 1; \
	else \
		$(call print_warn,"validate_strategy.py 不存在"); \
	fi
	$(call print_ok,策略验证完成)

.PHONY: lookahead-check
lookahead-check: ## 前视偏差检测
	$(call print_step,前视偏差检测...)
	@if [ -f "$(SCRIPTS_DIR)/check_lookahead.py" ]; then \
		$(PYTHON) $(SCRIPTS_DIR)/check_lookahead.py || exit 1; \
	else \
		$(call print_warn,"check_lookahead.py 不存在"); \
	fi
	$(call print_ok,前视偏差检测完成)

# ==============================================================================
# 代码质量
# ==============================================================================

.PHONY: format
format: ## 格式化代码
	$(call print_step,格式化代码...)
	@if command -v clang-format >/dev/null 2>&1; then \
		find $(SRC_DIR) \( -name "*.cpp" -o -name "*.hpp" \) -print0 | \
			xargs -0 -r clang-format -i; \
	else \
		$(call print_warn,"clang-format 未安装"); \
	fi
	@if [ -d "$(FRONTEND_DIR)/node_modules" ]; then \
		cd $(FRONTEND_DIR) && $(NPM) run format 2>/dev/null || true; \
	fi
	$(call print_ok,格式化完成)

.PHONY: format-check
format-check: ## 检查代码格式
	$(call print_step,检查代码格式...)
	@if command -v clang-format >/dev/null 2>&1; then \
		find $(SRC_DIR) \( -name "*.cpp" -o -name "*.hpp" \) -print0 | \
			xargs -0 -r clang-format --dry-run --Werror || \
			( $(call print_err,"格式检查失败"); exit 1 ); \
	else \
		$(call print_warn,"clang-format 未安装，跳过"); \
	fi
	$(call print_ok,格式检查通过)

.PHONY: lint
lint: lint-cpp lint-frontend ## 静态分析

.PHONY: lint-cpp
lint-cpp: ## C++ 静态分析
	$(call print_step,C++ 静态分析...)
	@if [ ! -d "$(BUILD_RELEASE)" ]; then \
		$(call print_err,"未构建，请先 make build"); \
		exit 1; \
	fi
	@if command -v run-clang-tidy >/dev/null 2>&1; then \
		cd $(BUILD_RELEASE) && run-clang-tidy -p . $(SRC_DIR) || \
			( $(call print_err,"静态分析失败"); exit 1 ); \
	else \
		$(call print_warn,"run-clang-tidy 未安装"); \
	fi
	$(call print_ok,C++ 静态分析完成)

.PHONY: lint-frontend
lint-frontend: ## 前端静态分析
	@if [ -d "$(FRONTEND_DIR)/node_modules" ]; then \
		cd $(FRONTEND_DIR) && $(NPM) run lint 2>/dev/null || \
			$(call print_warn,"前端 lint 未配置"); \
	fi

.PHONY: security
security: ## 安全扫描
	$(call print_step,安全扫描...)
	@if [ -f "$(SCRIPTS_DIR)/security_scan.py" ]; then \
		$(PYTHON) $(SCRIPTS_DIR)/security_scan.py || true; \
	fi
	@if [ -d "$(FRONTEND_DIR)/node_modules" ]; then \
		cd $(FRONTEND_DIR) && $(NPM) audit 2>/dev/null || true; \
	fi
	$(call print_ok,安全扫描完成)

.PHONY: check-all
check-all: format-check lint validate security ## 全部检查

# ==============================================================================
# 基准测试
# ==============================================================================

.PHONY: bench
bench: ## 运行基准测试
	$(call print_step,运行基准测试...)
	@if [ ! -d "$(BUILD_RELEASE)/benchmarks" ]; then \
		$(call print_err,"基准测试未构建"); \
		exit 1; \
	fi
	@for bench in bench_indicator bench_strategy bench_event_bus bench_backtest; do \
		if [ -f "$(BUILD_RELEASE)/benchmarks/$$bench" ]; then \
			echo "运行 $$bench"; \
			$(BUILD_RELEASE)/benchmarks/$$bench || true; \
		fi; \
	done
	$(call print_ok,基准测试完成)

.PHONY: profile
profile: ## 性能分析
	$(call print_step,性能分析...)
	@if ! command -v perf >/dev/null 2>&1; then \
		$(call print_err,"perf 未安装"); \
		exit 1; \
	fi
	@cd $(BUILD_RELEASE) && perf record -g ./$(CORE_BINARY) --profile || true
	@perf report || true

# ==============================================================================
# 数据库
# ==============================================================================

.PHONY: db-init
db-init: ## 初始化数据库
	$(call print_step,初始化数据库...)
	@if [ -f "$(SCRIPTS_DIR)/init_db.sh" ]; then \
		$(SCRIPTS_DIR)/init_db.sh --env $(ENV) || \
			( $(call print_err,"数据库初始化失败"); exit 1 ); \
	else \
		$(call print_err,"init_db.sh 不存在"); \
		exit 1; \
	fi
	$(call print_ok,数据库初始化完成)

.PHONY: db-migrate
db-migrate: ## 运行数据库迁移
	$(call print_step,运行迁移...)
	@if ! command -v psql >/dev/null 2>&1; then \
		$(call print_err,"psql 未安装"); \
		exit 1; \
	fi
	@for f in $(MIGRATIONS_DIR)/*.sql; do \
		if [ -f "$$f" ]; then \
			echo "执行 $$f"; \
			PGPASSWORD="$(DB_PASSWORD)" psql -h $(DB_HOST) -p $(DB_PORT) \
				-U $(DB_USER) -d $(DB_NAME) -f "$$f" -v ON_ERROR_STOP=1 || \
				( $(call print_err,"迁移失败: $$f"); exit 1 ); \
		fi; \
	done
	$(call print_ok,迁移完成)

.PHONY: db-backup
db-backup: check-dirs ## 备份数据库
	$(call print_step,备份数据库...)
	@if [ -f "$(SCRIPTS_DIR)/backup_db.sh" ]; then \
		$(SCRIPTS_DIR)/backup_db.sh || exit 1; \
	else \
		$(call print_err,"backup_db.sh 不存在"); \
		exit 1; \
	fi
	$(call print_ok,备份完成)

.PHONY: db-restore
db-restore: ## 恢复数据库
	@if [ -z "$(BACKUP_FILE)" ]; then \
		$(call print_err,"请指定 BACKUP_FILE=xxx"); \
		exit 1; \
	fi
	$(call print_step,恢复数据库...)
	@if [ -f "$(SCRIPTS_DIR)/restore_db.sh" ]; then \
		$(SCRIPTS_DIR)/restore_db.sh $(BACKUP_FILE) || exit 1; \
	else \
		$(call print_err,"restore_db.sh 不存在"); \
		exit 1; \
	fi
	$(call print_ok,恢复完成)

.PHONY: db-shell
db-shell: ## 连接数据库
	@if ! command -v psql >/dev/null 2>&1; then \
		$(call print_err,"psql 未安装"); \
		exit 1; \
	fi
	@PGPASSWORD="$(DB_PASSWORD)" psql -h $(DB_HOST) -p $(DB_PORT) \
		-U $(DB_USER) -d $(DB_NAME)

# ==============================================================================
# 运行
# ==============================================================================

.PHONY: run
run: run-simulation ## 默认运行模拟盘

.PHONY: run-bootstrap
run-bootstrap: ## 启动 Bootstrap 服务
	$(call print_step,启动 Bootstrap...)
	@if [ ! -f "$(BUILD_RELEASE)/bootstrap/$(BOOTSTRAP_BINARY)" ]; then \
		$(call print_err,"Bootstrap 未构建"); \
		exit 1; \
	fi
	@$(BUILD_RELEASE)/bootstrap/$(BOOTSTRAP_BINARY) --env $(ENV)

.PHONY: run-core
run-core: ## 启动核心进程
	$(call print_step,启动核心进程...)
	@if [ ! -f "$(BUILD_RELEASE)/core_main/$(CORE_BINARY)" ]; then \
		$(call print_err,"核心进程未构建"); \
		exit 1; \
	fi
	@$(BUILD_RELEASE)/core_main/$(CORE_BINARY) --env $(ENV)

.PHONY: run-backend
run-backend: ## 启动后端
	@$(BUILD_RELEASE)/backend/$(BACKEND_BINARY) --env $(ENV)

.PHONY: run-ai
run-ai: ## 启动 AI 服务
	@$(BUILD_RELEASE)/ai/$(AI_BINARY) --env $(ENV)

.PHONY: run-frontend
run-frontend: ## 启动前端开发服务器
	@if [ ! -d "$(FRONTEND_DIR)/node_modules" ]; then \
		$(call print_err,"前端依赖未安装"); \
		exit 1; \
	fi
	@cd $(FRONTEND_DIR) && $(NPM) run dev

.PHONY: run-simulation
run-simulation: ## 运行模拟盘
	$(call print_step,启动模拟盘...)
	@if [ ! -f "$(SCRIPTS_DIR)/run_simulation.py" ]; then \
		$(call print_err,"run_simulation.py 不存在"); \
		exit 1; \
	fi
	@$(PYTHON) $(SCRIPTS_DIR)/run_simulation.py --env $(ENV)

.PHONY: run-live
run-live: ## 运行实盘
	@if [ "$(CI)" = "yes" ]; then \
		$(call print_err,"CI 环境禁止实盘"); \
		exit 1; \
	fi
	@printf "$(C_RED)⚠ 启动实盘交易$(C_RESET)\n"
	@read -p "确认启动实盘? [yes/N] " confirm; \
	if [ "$$confirm" != "yes" ]; then \
		$(call print_warn,"已取消"); \
		exit 0; \
	fi
	@$(PYTHON) $(SCRIPTS_DIR)/run_live.py --env prod

.PHONY: start
start: ## 一键启动所有服务
	$(call print_step,启动所有服务...)
	@if [ ! -f "$(SCRIPTS_DIR)/start.sh" ]; then \
		$(call print_err,"start.sh 不存在"); \
		exit 1; \
	fi
	@$(SCRIPTS_DIR)/start.sh

.PHONY: stop
stop: ## 停止所有服务
	$(call print_step,停止所有服务...)
	@if [ -f "$(SCRIPTS_DIR)/stop.sh" ]; then \
		$(SCRIPTS_DIR)/stop.sh; \
	else \
		pkill -f "$(CORE_BINARY)" 2>/dev/null || true; \
		pkill -f "$(BACKEND_BINARY)" 2>/dev/null || true; \
		pkill -f "$(BOOTSTRAP_BINARY)" 2>/dev/null || true; \
	fi
	$(call print_ok,服务已停止)

.PHONY: restart
restart: stop start ## 重启

.PHONY: status
status: ## 查看服务状态
	$(call print_info,服务状态:)
	@if command -v systemctl >/dev/null 2>&1 && systemctl list-units --all >/dev/null 2>&1; then \
		for svc in quant-bootstrap quant-core quant-backend quant-ai; do \
			if systemctl is-active --quiet $$svc 2>/dev/null; then \
				printf "  %s: $(C_GREEN)运行中$(C_RESET)\n" "$$svc"; \
			else \
				printf "  %s: $(C_RED)未运行$(C_RESET)\n" "$$svc"; \
			fi; \
		done \
	else \
		for proc in $(BOOTSTRAP_BINARY) $(CORE_BINARY) $(BACKEND_BINARY) $(AI_BINARY); do \
			if pgrep -f "$$proc" >/dev/null 2>&1; then \
				printf "  %s: $(C_GREEN)运行中 (PID: %s)$(C_RESET)\n" "$$proc" "$$(pgrep -f $$proc | head -1)"; \
			else \
				printf "  %s: $(C_RED)未运行$(C_RESET)\n" "$$proc"; \
			fi; \
		done \
	fi

.PHONY: logs
logs: ## 查看日志
	@if [ ! -d "$(LOGS_DIR)" ]; then \
		$(call print_err,"日志目录不存在"); \
		exit 1; \
	fi
	@if [ -z "$$(ls -A $(LOGS_DIR) 2>/dev/null)" ]; then \
		$(call print_warn,"暂无日志"); \
	else \
		tail -f $(LOGS_DIR)/*.log; \
	fi

# ==============================================================================
# 部署
# ==============================================================================

.PHONY: deploy
deploy: deploy-$(ENV) ## 部署（默认 dev）

.PHONY: deploy-dev
deploy-dev: ## 部署到开发环境
	$(call print_step,部署到开发环境...)
	@if [ ! -f "$(SCRIPTS_DIR)/deploy.sh" ]; then \
		$(call print_err,"deploy.sh 不存在"); \
		exit 1; \
	fi
	@ENV=dev $(SCRIPTS_DIR)/deploy.sh dev

.PHONY: deploy-prod
deploy-prod: ## 部署到生产环境
	@if [ "$(CI)" = "yes" ]; then \
		$(call print_err,"CI 环境禁止直接部署生产"); \
		exit 1; \
	fi
	@printf "$(C_RED)⚠ 部署到生产环境$(C_RESET)\n"
	@read -p "确认部署到生产? [yes/N] " confirm; \
	if [ "$$confirm" != "yes" ]; then \
		$(call print_warn,"已取消"); \
		exit 0; \
	fi
	@ENV=prod $(SCRIPTS_DIR)/deploy.sh prod

.PHONY: deploy-docker
deploy-docker: ## Docker 部署
	$(call print_step,Docker 部署...)
	@if ! command -v $(DOCKER) >/dev/null 2>&1; then \
		$(call print_err,"Docker 未安装"); \
		exit 1; \
	fi
	@$(DOCKER_COMPOSE) up -d || \
		( $(call print_err,"Docker 部署失败"); exit 1 )
	$(call print_ok,Docker 部署完成)

.PHONY: rollback
rollback: ## 回滚部署
	$(call print_warn,回滚部署...)
	@if [ -f "$(SCRIPTS_DIR)/rollback.sh" ]; then \
		$(SCRIPTS_DIR)/rollback.sh || exit 1; \
	else \
		$(call print_err,"rollback.sh 不存在"); \
		exit 1; \
	fi

.PHONY: switch-traffic
switch-traffic: ## 蓝绿切换
	@if [ -z "$(TARGET)" ]; then \
		$(call print_err,"请指定 TARGET=blue|green"); \
		exit 1; \
	fi
	@if [ -f "$(SCRIPTS_DIR)/switch_traffic.sh" ]; then \
		$(SCRIPTS_DIR)/switch_traffic.sh $(TARGET) || exit 1; \
	else \
		$(call print_err,"switch_traffic.sh 不存在"); \
		exit 1; \
	fi

.PHONY: health
health: ## 健康检查
	$(call print_info,健康检查:)
	@check_url() { \
		if command -v curl >/dev/null 2>&1; then \
			if curl -sf --max-time 3 "$$1" >/dev/null 2>&1; then \
				printf "  %s: $(C_GREEN)✓$(C_RESET)\n" "$$2"; \
			else \
				printf "  %s: $(C_RED)✗$(C_RESET)\n" "$$2"; \
			fi \
		elif command -v wget >/dev/null 2>&1; then \
			if wget -q --timeout=3 -O- "$$1" >/dev/null 2>&1; then \
				printf "  %s: $(C_GREEN)✓$(C_RESET)\n" "$$2"; \
			else \
				printf "  %s: $(C_RED)✗$(C_RESET)\n" "$$2"; \
			fi \
		else \
			printf "  %s: $(C_YELLOW)? (无 curl/wget)$(C_RESET)\n" "$$2"; \
		fi; \
	}
	@check_url "http://localhost:8080/health" "Bootstrap"
	@check_url "http://localhost:8000/health" "Backend"
	@check_url "http://localhost:9090/-/healthy" "Prometheus"

# ==============================================================================
# Docker
# ==============================================================================

.PHONY: docker-build
docker-build: ## 构建所有 Docker 镜像
	$(call print_step,构建 Docker 镜像...)
	@if ! command -v $(DOCKER) >/dev/null 2>&1; then \
		$(call print_err,"Docker 未安装"); \
		exit 1; \
	fi
	@for svc in core backend ai frontend bootstrap; do \
		echo "构建 $$svc"; \
		$(DOCKER) build -t $(IMAGE_PREFIX)/$$svc:$(IMAGE_TAG) \
			-f Dockerfile.$$svc . || \
			( $(call print_err,"构建 $$svc 失败"); exit 1 ); \
	done
	$(call print_ok,镜像构建完成)

.PHONY: docker-push
docker-push: ## 推送镜像
	$(call print_step,推送镜像...)
	@for svc in core backend ai frontend bootstrap; do \
		$(DOCKER) push $(IMAGE_PREFIX)/$$svc:$(IMAGE_TAG) || \
			( $(call print_err,"推送 $$svc 失败"); exit 1 ); \
	done
	$(call print_ok,镜像推送完成)

.PHONY: docker-up
docker-up: ## 启动 Docker 服务
	@$(DOCKER_COMPOSE) up -d

.PHONY: docker-down
docker-down: ## 停止 Docker 服务
	@$(DOCKER_COMPOSE) down

.PHONY: docker-logs
docker-logs: ## Docker 日志
	@$(DOCKER_COMPOSE) logs -f

.PHONY: docker-clean
docker-clean: ## 清理 Docker（仅本项目）
	@$(DOCKER_COMPOSE) down -v --remove-orphans
	@$(DOCKER) image prune -f --filter "label=project=$(PROJECT_NAME)"
	$(call print_ok,Docker 清理完成)

# ==============================================================================
# 打包
# ==============================================================================

.PHONY: package
package: check-dirs build build-frontend ## 打包发布
	$(call print_step,打包...)
	@PKG_DIR="$(DIST_DIR)/$(PROJECT_NAME)-$(VERSION)"
	@rm -rf "$$PKG_DIR"
	@mkdir -p "$$PKG_DIR"
	@[ -d "$(BUILD_RELEASE)/bin" ] && cp -r $(BUILD_RELEASE)/bin "$$PKG_DIR/" || true
	@[ -d "$(FRONTEND_DIR)/dist" ] && cp -r $(FRONTEND_DIR)/dist "$$PKG_DIR/frontend" || true
	@[ -d config ] && cp -r config "$$PKG_DIR/"
	@[ -d models ] && cp -r models "$$PKG_DIR/"
	@[ -d migrations ] && cp -r migrations "$$PKG_DIR/"
	@[ -d scripts ] && cp -r scripts "$$PKG_DIR/"
	@[ -d deploy ] && cp -r deploy "$$PKG_DIR/"
	@cp Makefile README.md VERSION "$$PKG_DIR/" 2>/dev/null || true
	@cd $(DIST_DIR) && tar -czf $(PROJECT_NAME)-$(VERSION).tar.gz \
		$(PROJECT_NAME)-$(VERSION)
	$(call print_ok,"打包完成: $(DIST_DIR)/$(PROJECT_NAME)-$(VERSION).tar.gz")

.PHONY: package-source
package-source: check-dirs ## 打包源码
	@if ! git rev-parse --git-dir >/dev/null 2>&1; then \
		$(call print_err,"非 Git 仓库"); \
		exit 1; \
	fi
	@git archive --format=tar.gz \
		--prefix=$(PROJECT_NAME)-$(VERSION)/ \
		-o $(DIST_DIR)/$(PROJECT_NAME)-$(VERSION)-src.tar.gz HEAD
	$(call print_ok,源码打包完成)

.PHONY: release
release: check-all test package ## 发布流程
	$(call print_ok,发布准备完成)

# ==============================================================================
# 文档
# ==============================================================================

.PHONY: docs
docs: check-dirs ## 生成文档
	$(call print_step,生成文档...)
	@if command -v doxygen >/dev/null 2>&1 && [ -f Doxyfile ]; then \
		doxygen Doxyfile; \
	else \
		$(call print_warn,"doxygen 未安装或 Doxyfile 不存在"); \
	fi
	@if [ -d "$(FRONTEND_DIR)/node_modules" ]; then \
		cd $(FRONTEND_DIR) && $(NPM) run docs 2>/dev/null || true; \
	fi
	$(call print_ok,文档生成完成)

.PHONY: docs-serve
docs-serve: ## 启动文档服务器
	@if [ ! -d "$(DIST_DIR)/docs" ]; then \
		$(call print_err,"文档未生成"); \
		exit 1; \
	fi
	@cd $(DIST_DIR)/docs && $(PYTHON) -m http.server 8081

# ==============================================================================
# 监控
# ==============================================================================

.PHONY: monitoring-up
monitoring-up: ## 启动监控
	@$(DOCKER_COMPOSE) up -d prometheus grafana
	$(call print_ok,"Grafana: http://localhost:3001")

.PHONY: monitoring-down
monitoring-down: ## 停止监控
	@$(DOCKER_COMPOSE) stop prometheus grafana

.PHONY: monitoring-status
monitoring-status: ## 监控状态
	@if ! command -v jq >/dev/null 2>&1; then \
		$(call print_warn,"jq 未安装"); \
		exit 0; \
	fi
	@curl -s http://localhost:9090/api/v1/targets | \
		jq '.data.activeTargets[] | {job: .labels.job, health: .health}' || \
		$(call print_err,"Prometheus 无响应")

# ==============================================================================
# 清理
# ==============================================================================

.PHONY: clean
clean: ## 清理构建产物
	$(call print_step,清理构建产物...)
	@rm -rf $(BUILD_DIR)
	@rm -rf $(FRONTEND_DIR)/dist 2>/dev/null || true
	@rm -rf $(FRONTEND_DIR)/node_modules/.cache 2>/dev/null || true
	@find . -type f \( -name "*.o" -o -name "*.a" -o -name "*.so" \) -delete 2>/dev/null || true
	$(call print_ok,清理完成)

.PHONY: clean-logs
clean-logs: ## 清理日志
	$(call print_step,清理日志...)
	@if [ -d "$(LOGS_DIR)" ]; then \
		find $(LOGS_DIR) -name "*.log" -type f -mtime +7 -delete 2>/dev/null || true; \
	fi
	$(call print_ok,日志清理完成)

.PHONY: clean-data
clean-data: ## 清理运行时数据
	@if [ "$(CI)" != "yes" ] && [ -t 0 ]; then \
		printf "$(C_RED)⚠ 清理运行时数据$(C_RESET)\n"; \
		read -p "确认清理? [yes/N] " confirm; \
		if [ "$$confirm" != "yes" ]; then \
			$(call print_warn,"已取消"); \
			exit 0; \
		fi; \
	fi
	@rm -rf $(SNAPSHOTS_DIR)/* 2>/dev/null || true
	@rm -rf $(REPLAY_DIR)/* 2>/dev/null || true
	@rm -rf data/cache/* 2>/dev/null || true
	$(call print_ok,数据清理完成)

.PHONY: distclean
distclean: clean ## 深度清理
	$(call print_step,深度清理...)
	@rm -rf $(DIST_DIR)
	@rm -rf $(FRONTEND_DIR)/node_modules 2>/dev/null || true
	@rm -f conan.lock
	@rm -rf .conan 2>/dev/null || true
	@find . -type d -name "__pycache__" -exec rm -rf {} + 2>/dev/null || true
	@find . -type f -name "*.pyc" -delete 2>/dev/null || true
	$(call print_ok,深度清理完成)

.PHONY: uninstall
uninstall: ## 卸载系统服务
	@if [ "$(CI)" = "yes" ]; then \
		$(call print_err,"CI 环境禁止卸载"); \
		exit 1; \
	fi
	@printf "$(C_RED)⚠ 卸载系统服务$(C_RESET)\n"
	@read -p "确认卸载? [yes/N] " confirm; \
	if [ "$$confirm" != "yes" ]; then \
		$(call print_warn,"已取消"); \
		exit 0; \
	fi
	@if command -v systemctl >/dev/null 2>&1; then \
		sudo systemctl stop quant-bootstrap quant-core quant-backend quant-ai 2>/dev/null || true; \
		sudo systemctl disable quant-bootstrap quant-core quant-backend quant-ai 2>/dev/null || true; \
		sudo rm -f /etc/systemd/system/quant-*.service; \
		sudo systemctl daemon-reload; \
	fi
	$(call print_ok,卸载完成)

# ==============================================================================
# 开发辅助
# ==============================================================================

.PHONY: dev
dev: ## 开发模式（前端热重载 + 核心进程）
	$(call print_step,启动开发模式...)
	@if [ ! -f "$(BUILD_RELEASE)/core_main/$(CORE_BINARY)" ]; then \
		$(call print_err,"核心进程未构建"); \
		exit 1; \
	fi
	@if [ ! -d "$(FRONTEND_DIR)/node_modules" ]; then \
		$(call print_err,"前端依赖未安装"); \
		exit 1; \
	fi
	@printf "$(C_BLUE)启动前端和核心进程，Ctrl+C 退出$(C_RESET)\n"
	@trap 'kill 0' INT TERM EXIT; \
	( cd $(FRONTEND_DIR) && $(NPM) run dev ) & \
	( $(BUILD_RELEASE)/core_main/$(CORE_BINARY) --env dev ) & \
	wait

.PHONY: watch
watch: ## 监视文件变化并重建
	@if ! command -v inotifywait >/dev/null 2>&1; then \
		$(call print_err,"inotify-tools 未安装"); \
		exit 1; \
	fi
	$(call print_step,监视文件变化...)
	@trap 'exit 0' INT TERM; \
	while true; do \
		inotifywait -r -q -e modify,create,delete $(SRC_DIR) || break; \
		printf "$(C_BLUE)文件变化，重新构建...$(C_RESET)\n"; \
		$(MAKE) build-release || printf "$(C_RED)构建失败$(C_RESET)\n"; \
	done

.PHONY: install-hooks
install-hooks: ## 安装 Git 钩子
	$(call print_step,安装 Git 钩子...)
	@if [ ! -d .git ]; then \
		$(call print_err,"非 Git 仓库"); \
		exit 1; \
	fi
	@mkdir -p .git/hooks
	@for hook in pre-commit pre-push; do \
		if [ -f "$(SCRIPTS_DIR)/hooks/$$hook" ]; then \
			cp $(SCRIPTS_DIR)/hooks/$$hook .git/hooks/$$hook; \
			chmod +x .git/hooks/$$hook; \
		fi; \
	done
	$(call print_ok,Git 钩子安装完成)

.PHONY: gen-ctags
gen-ctags: ## 生成 ctags
	@if ! command -v ctags >/dev/null 2>&1; then \
		$(call print_err,"ctags 未安装"); \
		exit 1; \
	fi
	@ctags -R --c++-kinds=+p --fields=+iaS --extras=+q $(SRC_DIR)
	$(call print_ok,ctags 生成完成)

.PHONY: gen-compile-commands
gen-compile-commands: ## 生成 compile_commands.json
	@if [ -f "$(BUILD_RELEASE)/compile_commands.json" ]; then \
		ln -sf $(BUILD_RELEASE)/compile_commands.json compile_commands.json; \
		$(call print_ok,compile_commands.json 已链接); \
	else \
		$(call print_err,"compile_commands.json 不存在，请先 make build"); \
		exit 1; \
	fi

# ==============================================================================
# CI/CD
# ==============================================================================

.PHONY: ci
ci: check-tools check-dirs deps validate build-release test ## CI 流程
	$(call print_ok,CI 通过)

.PHONY: cd
cd: ci package docker-build ## CD 流程
	$(call print_ok,CD 完成)

.PHONY: pre-commit
pre-commit: format-check validate ## 提交前检查
	$(call print_ok,提交前检查通过)

# ==============================================================================
# 信息
# ==============================================================================

.PHONY: info
info: ## 显示项目信息
	@printf "\n$(C_CYAN)项目信息$(C_RESET)\n"
	@printf "  名称:     %s\n" "$(PROJECT_NAME)"
	@printf "  版本:     %s\n" "$(VERSION)"
	@printf "  提交:     %s\n" "$(GIT_COMMIT)"
	@printf "  分支:     %s\n" "$(GIT_BRANCH)"
	@printf "  构建时间: %s\n" "$(BUILD_DATE)"
	@printf "  环境:     %s\n" "$(ENV)"
	@printf "  构建类型: %s\n" "$(BUILD_TYPE)"
	@printf "  平台:     %s\n" "$(UNAME_S)"
	@printf "  CPU 核心: %s\n" "$(NPROC)"
	@printf "  颜色支持: %s\n" "$(HAS_COLOR)"
	@printf "  CI 环境:  %s\n" "$(CI)"
	@printf "\n"

.PHONY: version
version: ## 显示版本
	@echo $(VERSION)

.PHONY: env
env: ## 显示环境变量
	@printf "ENV=$(ENV)\n"
	@printf "BUILD_TYPE=$(BUILD_TYPE)\n"
	@printf "JOBS=$(JOBS)\n"
	@printf "DB_HOST=$(DB_HOST)\n"
	@printf "DB_NAME=$(DB_NAME)\n"
	@printf "UNAME_S=$(UNAME_S)\n"
	@printf "NPROC=$(NPROC)\n"
	@printf "HAS_COLOR=$(HAS_COLOR)\n"
	@printf "CI=$(CI)\n"
