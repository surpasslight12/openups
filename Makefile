CC ?= gcc
# C23 标准 + 安全和性能优化标志
CFLAGS ?= -O3 -std=c2x -Wall -Wextra -Wpedantic -Werror=implicit-function-declaration \
          -Werror=format-security -Wformat=2 -Wstrict-overflow=5 \
          -fstack-protector-strong -fPIE -D_FORTIFY_SOURCE=3 \
          -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
          -march=native -mtune=native -flto
LDFLAGS ?= -Wl,-z,relro,-z,now -Wl,-z,noexecstack -pie -flto

BIN_DIR := bin
SRC_DIR := src
TARGET := $(BIN_DIR)/openups

# 源文件
SRCS := $(SRC_DIR)/main.c \
        $(SRC_DIR)/common.c \
        $(SRC_DIR)/logger.c \
        $(SRC_DIR)/config.c \
        $(SRC_DIR)/icmp.c \
        $(SRC_DIR)/systemd.c \
        $(SRC_DIR)/monitor.c

OBJS := $(SRCS:$(SRC_DIR)/%.c=$(BIN_DIR)/%.o)

.PHONY: all clean run install uninstall test help

all: $(TARGET)

test: all
	@echo "Running test suite..."
	@./test.sh

$(BIN_DIR):
	@mkdir -p $(BIN_DIR)

$(BIN_DIR)/%.o: $(SRC_DIR)/%.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@
	@echo "Build complete: $(TARGET)"

clean:
	rm -rf $(BIN_DIR)

run: $(TARGET)
	@echo "Running $(TARGET) in dry-run mode..."
	sudo $(TARGET) --target 127.0.0.1 --interval 1 --threshold 2 --dry-run --verbose

# 安装到系统
install: $(TARGET)
	@echo "Installing $(TARGET) to /usr/local/bin..."
	sudo cp $(TARGET) /usr/local/bin/openups
	sudo chmod 755 /usr/local/bin/openups
	sudo setcap cap_net_raw+ep /usr/local/bin/openups || true
	@echo "Installation complete. Run 'openups --help' for usage."

# 卸载
uninstall:
	@echo "Uninstalling openups from /usr/local/bin..."
	sudo rm -f /usr/local/bin/openups
	@echo "Uninstall complete."

# 帮助信息
help:
	@echo "OpenUPS C Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  all        - Build the project (default)"
	@echo "  clean      - Remove build artifacts"
	@echo "  run        - Build and run in test mode"
	@echo "  install    - Install to /usr/local/bin"
	@echo "  uninstall  - Remove from /usr/local/bin"
	@echo "  help       - Show this help message"
	@echo ""
	@echo "Variables:"
	@echo "  CC         - C compiler (default: gcc)"
	@echo "  CFLAGS     - Compiler flags"
	@echo "  LDFLAGS    - Linker flags"
	@echo ""
	@echo "Examples:"
	@echo "  make                    # Build with defaults"
	@echo "  make CC=clang           # Build with clang"
	@echo "  make CFLAGS=\"-g -O0\"    # Build debug version"
