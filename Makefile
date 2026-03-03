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

.PHONY: all clean

all: $(TARGET)

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
