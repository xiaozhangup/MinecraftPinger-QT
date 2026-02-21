SHELL := /usr/bin/env bash

# 可按需覆盖：
#   make CONFIG=Release
#   make BUILD_DIR=build-release
#   make APP=YourTargetName run
BUILD_DIR ?= build
CONFIG ?= Debug
APP ?= MinecraftPinger

CMAKE ?= cmake
CTEST ?= ctest

# 额外 cmake 配置参数：make CMAKE_ARGS="-Dxxx=ON"
CMAKE_ARGS ?=

# 并行编译 job 数：make JOBS=8
JOBS ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

.PHONY: help configure reconfigure build run test clean

help:
	@echo "Targets:"
	@echo "  make build        # 配置并构建"
	@echo "  make run          # 构建后运行（./$(BUILD_DIR)/$(APP)）"
	@echo "  make test         # 构建后运行 ctest"
	@echo "  make clean        # 删除构建目录"
	@echo "  make reconfigure  # 重新生成 CMakeCache"
	@echo
	@echo "Vars (override): BUILD_DIR=$(BUILD_DIR) CONFIG=$(CONFIG) APP=$(APP)"
	@echo "Examples: make CONFIG=Release build | make BUILD_DIR=build-rel run"

# 生成 build/CMakeCache.txt 作为 configure 的标志
$(BUILD_DIR)/CMakeCache.txt: CMakeLists.txt
	@mkdir -p "$(BUILD_DIR)"
	@echo "[cmake] configure -> $(BUILD_DIR) (CONFIG=$(CONFIG))"
	@"$(CMAKE)" -S . -B "$(BUILD_DIR)" -DCMAKE_BUILD_TYPE="$(CONFIG)" $(CMAKE_ARGS)

configure: $(BUILD_DIR)/CMakeCache.txt

reconfigure:
	@rm -f "$(BUILD_DIR)/CMakeCache.txt"
	@$(MAKE) configure

build: configure
	@echo "[cmake] build -> $(BUILD_DIR) (JOBS=$(JOBS))"
	@"$(CMAKE)" --build "$(BUILD_DIR)" -j "$(JOBS)"

run: build
	@echo "[run] ./$(BUILD_DIR)/$(APP)"
	@"./$(BUILD_DIR)/$(APP)"

test: build
	@echo "[ctest] $(BUILD_DIR)"
	@"$(CTEST)" --test-dir "$(BUILD_DIR)" --output-on-failure -C "$(CONFIG)"

clean:
	@echo "[clean] rm -rf $(BUILD_DIR)"
	@rm -rf "$(BUILD_DIR)"
