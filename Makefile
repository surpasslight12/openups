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
          -march=native -mtune=native -flto=auto -pipe \
          -ffunction-sections -fdata-sections -fmerge-all-constants \
          -fno-plt -fno-semantic-interposition -fno-common \
          -fvisibility=hidden

# 自动生成头文件依赖（避免增量构建遗漏头文件变化，导致 -flto 下结构体布局不一致）
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

CLANG_FORMAT ?= clang-format
FORMAT_SRCS := $(wildcard $(SRC_DIR)/*.c $(SRC_DIR)/*.h)

# 源文件（优化顺序：按依赖关系排序）
SRCS := $(SRC_DIR)/base.c \
	$(SRC_DIR)/config.c \
	$(SRC_DIR)/icmp.c \
	$(SRC_DIR)/integrations.c \
	$(SRC_DIR)/context.c \
	$(SRC_DIR)/main.c

OBJS := $(SRCS:$(SRC_DIR)/%.c=$(BIN_DIR)/%.o)
DEPS := $(OBJS:.o=.d)

.PHONY: all clean run install uninstall test format check-format help

all: $(TARGET)

-include $(DEPS)

test: all
	@echo "Running test suite..."
	@./test.sh

format:
	@$(CLANG_FORMAT) -i $(FORMAT_SRCS)
	@echo "Formatted: $(FORMAT_SRCS)"

check-format:
	@$(CLANG_FORMAT) --dry-run -Werror $(FORMAT_SRCS)
	@echo "Format check passed"

$(BIN_DIR):
	@mkdir -p $(BIN_DIR)

$(BIN_DIR)/%.o: $(SRC_DIR)/%.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@
	@echo "Build complete: $(TARGET)"

clean:
	rm -rf $(BIN_DIR)

strip: $(TARGET)
	strip --strip-all $(TARGET)
	@echo "Strip complete: $(TARGET)"
	@ls -lh $(TARGET)

run: $(TARGET)
	@echo "Running $(TARGET) in dry-run mode..."
	sudo $(TARGET) --target 127.0.0.1 --interval 1 --threshold 2 --dry-run --log-level debug

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
	@echo "========================================"
	@echo "OpenUPS - Network Monitor with Auto-Shutdown"
	@echo "========================================"
	@echo ""
	@echo "Usage: make [target] [variables]"
	@echo ""
	@echo "Targets:"
	@echo "  all        - Build the project (default target)"
	@echo "  clean      - Remove build artifacts (bin/ directory)"
	@echo "  test       - Build and run automated test suite"
	@echo "  format     - Format src/*.c and src/*.h using clang-format"
	@echo "  check-format - Verify formatting (clang-format --dry-run -Werror)"
	@echo "  run        - Build and run in test mode with localhost"
	@echo "  install    - Install binary to /usr/local/bin and set CAP_NET_RAW"
	@echo "  uninstall  - Remove binary from /usr/local/bin"
	@echo "  help       - Display this help message"
	@echo ""
	@echo "Variables (optional):"
	@echo "  CC         - C compiler executable (default: gcc)"
	@echo "  CFLAGS     - Compiler optimization and safety flags"
	@echo "  LDFLAGS    - Linker security and optimization flags"
	@echo "  CLANG_FORMAT - clang-format executable (default: clang-format)"
	@echo ""
	@echo "Examples:"
	@echo "  make                       # Build with optimizations"
	@echo "  make format                # Apply code formatter"
	@echo "  make check-format          # Check formatting in CI"
	@echo "  make clean && make test    # Clean build and run tests"
	@echo "  make CC=clang              # Build with clang instead of gcc"
	@echo "  make install               # Install system-wide"
	@echo "  make CFLAGS=\"-g -O0\" clean make  # Debug build"
	@echo ""
	@echo "After installation:"
	@echo "  openups --help             # Show program usage"
	@echo "  openups --version          # Show version info"
	@echo "  systemctl start openups    # Start systemd service"
	@echo ""
	@echo "Documentation:"
	@echo "  README.md         - Project overview and features"
	@echo "  TECHNICAL.md      - Architecture and development guide"
	@echo "  CONTRIBUTING.md   - Contribution guidelines"
