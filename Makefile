CC ?= gcc

# 编译标志说明：
#   优化标志: -O3 -march=native -mtune=native -flto=auto
#   C 标准: -std=c23 (ISO C23)
#   代码检查: -Wall -Wextra -Wpedantic -Wshadow -Wnull-dereference -Wdouble-promotion
#   错误级: -Werror=implicit-function-declaration -Werror=format-security -Wformat=2
#   栈溢出防护: -Wstrict-overflow=5
#   安全加固: -fstack-protector-strong -fPIE -D_FORTIFY_SOURCE=3
#   POSIX 特性: -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
#   体积优化: -ffunction-sections -fdata-sections -fmerge-all-constants
#   性能优化: -fno-plt -fno-semantic-interposition -fno-common -pipe
#   符号可见性: -fvisibility=hidden（减小二进制体积、改善优化效果）
#
CFLAGS ?= -O3 -std=c23 -Wall -Wextra -Wpedantic \
          -Wshadow -Wnull-dereference -Wdouble-promotion \
          -Werror=implicit-function-declaration \
          -Werror=format-security -Wformat=2 -Wstrict-overflow=5 \
          -fstack-protector-strong -fPIE -D_FORTIFY_SOURCE=3 \
          -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
          -flto=auto -pipe \
          -ffunction-sections -fdata-sections -fmerge-all-constants \
          -fno-plt -fno-semantic-interposition -fno-common \
          -fvisibility=hidden

# 本机优化标志（仅在本机构建时使用，不可分发）
ifneq ($(PORTABLE),1)
  CFLAGS += -march=native -mtune=native
endif

# 自动生成头文件依赖
CFLAGS += -MMD -MP

# 链接标志说明：
#   RELRO: -Wl,-z,relro,-z,now (Full RELRO - 只读重定位表)
#   NX: -Wl,-z,noexecstack (禁用可执行栈)
#   PIE: -pie (位置无关可执行文件)
#   LTO: -flto=auto (并行链接时优化)
#   GC: -Wl,--gc-sections (配合 -ffunction-sections 移除未使用代码)
#   按需链接: -Wl,--as-needed (仅链接实际使用的共享库)
#   链接器优化: -Wl,-O2 (启用链接器内部优化)
#   符号哈希: -Wl,--hash-style=gnu (更快的符号查找)
#
LDFLAGS ?= -Wl,-z,relro,-z,now -Wl,-z,noexecstack -pie -flto=auto \
           -Wl,--gc-sections -Wl,--as-needed -Wl,-O2 -Wl,--hash-style=gnu

BIN_DIR := bin
SRC_DIR := src
TARGET := $(BIN_DIR)/openups

# 源文件（单文件架构）
SRCS := $(SRC_DIR)/main.c

# 对象文件：保持子目录结构
OBJS := $(patsubst $(SRC_DIR)/%.c,$(BIN_DIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

# ---- 安装路径 ----
BIN_INSTALL_PATH     := /usr/local/bin/openups
SERVICE_NAME         := openups.service
SERVICE_INSTALL_PATH := /etc/systemd/system/$(SERVICE_NAME)
ENV_FILE             := /etc/default/openups

.PHONY: all clean release install configure update uninstall

all: $(TARGET)

release: $(TARGET)
	strip --strip-all $(TARGET)
	@echo "Release build complete (stripped): $(TARGET)"

# ---- 安装 ----
# 完整安装：编译 → 安装二进制 → 交互配置 → 注册 systemd 服务
install: release
	@[ "$$(id -u)" -eq 0 ] || { echo "ERROR: Run as root (sudo make install)"; exit 1; }
	@echo "==> Installing binary to $(BIN_INSTALL_PATH)..."
	@cp $(TARGET) $(BIN_INSTALL_PATH)
	@chmod 755 $(BIN_INSTALL_PATH)
	@echo "==> Assigning CAP_NET_RAW capability..."
	@setcap cap_net_raw+ep $(BIN_INSTALL_PATH) || echo "WARNING: setcap failed (install libcap2-bin)"
	@$(MAKE) --no-print-directory configure
	@echo "==> Installing systemd service..."
	@cp systemd/$(SERVICE_NAME) $(SERVICE_INSTALL_PATH)
	@systemctl daemon-reload
	@systemctl enable $(SERVICE_NAME)
	@printf "Start service now? (y/n) [y]: "; read ans; \
	  ans=$${ans:-y}; \
	  if [ "$$ans" = "y" ] || [ "$$ans" = "Y" ]; then \
	    systemctl start $(SERVICE_NAME) && systemctl status $(SERVICE_NAME) --no-pager; \
	  fi
	@echo "==> Installation complete!"

# ---- 交互配置 ----
# 写入 $(ENV_FILE)；可单独运行以更新配置后重启服务
configure:
	@[ "$$(id -u)" -eq 0 ] || { echo "ERROR: Run as root (sudo make configure)"; exit 1; }
	@echo "==> Configuring parameters (press Enter to accept defaults)"
	@printf "Target IP to monitor [1.1.1.1]: "; read TARGET; TARGET=$${TARGET:-1.1.1.1}; \
	  printf "Check interval in seconds [10]: "; read INTERVAL; INTERVAL=$${INTERVAL:-10}; \
	  printf "Failure threshold [5]: "; read THRESHOLD; THRESHOLD=$${THRESHOLD:-5}; \
	  printf "Shutdown mode (immediate/delayed/log-only) [log-only]: "; read SMODE; SMODE=$${SMODE:-log-only}; \
	  printf "Dry run (true/false) [true]: "; read DRUN; DRUN=$${DRUN:-true}; \
	  if [ "$$DRUN" = "true" ]; then \
	    echo "WARNING: Dry Run ENABLED — will NOT actually shut down."; \
	  elif [ "$$SMODE" != "log-only" ]; then \
	    echo "WARNING: Dry Run DISABLED — will shut down if $$TARGET fails!"; \
	  fi; \
	  printf "# OpenUPS Environment File\nOPENUPS_TARGET=$$TARGET\nOPENUPS_INTERVAL=$$INTERVAL\nOPENUPS_THRESHOLD=$$THRESHOLD\nOPENUPS_TIMEOUT=2000\nOPENUPS_PAYLOAD_SIZE=56\nOPENUPS_DRY_RUN=$$DRUN\nOPENUPS_SHUTDOWN_MODE=$$SMODE\nOPENUPS_LOG_LEVEL=info\nOPENUPS_TIMESTAMP=false\nOPENUPS_SYSTEMD=true\nOPENUPS_WATCHDOG=false\n" > $(ENV_FILE); \
	  chmod 644 $(ENV_FILE); \
	  echo "==> Configuration saved to $(ENV_FILE)"

# ---- 更新 ----
# 重新编译并替换二进制 + 服务文件；不重置配置
update: release
	@[ "$$(id -u)" -eq 0 ] || { echo "ERROR: Run as root (sudo make update)"; exit 1; }
	@echo "==> Updating binary..."
	@cp $(TARGET) $(BIN_INSTALL_PATH)
	@chmod 755 $(BIN_INSTALL_PATH)
	@setcap cap_net_raw+ep $(BIN_INSTALL_PATH) || echo "WARNING: setcap failed"
	@echo "==> Updating service file..."
	@cp systemd/$(SERVICE_NAME) $(SERVICE_INSTALL_PATH)
	@systemctl daemon-reload
	@systemctl restart $(SERVICE_NAME) && systemctl status $(SERVICE_NAME) --no-pager || true
	@echo "==> Update complete!"

# ---- 卸载 ----
uninstall:
	@[ "$$(id -u)" -eq 0 ] || { echo "ERROR: Run as root (sudo make uninstall)"; exit 1; }
	@echo "==> Uninstalling OpenUPS..."
	@systemctl disable --now $(SERVICE_NAME) 2>/dev/null || true
	@rm -f $(BIN_INSTALL_PATH) $(SERVICE_INSTALL_PATH) $(ENV_FILE)
	@systemctl daemon-reload
	@echo "==> Uninstallation complete."

-include $(DEPS)

$(BIN_DIR):
	@mkdir -p $(BIN_DIR)

# 编译规则：支持子目录结构
$(BIN_DIR)/%.o: $(SRC_DIR)/%.c | $(BIN_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@
	@echo "Build complete: $(TARGET)"

clean:
	rm -rf $(BIN_DIR)
