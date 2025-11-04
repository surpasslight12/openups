# OpenUPS - AI Coding Instructions

OpenUPS 是一个高性能的 Linux 网络监控工具，通过 ICMP ping 检测网络可达性并在失败时执行关机策略。使用 C23 标准，零第三方依赖，深度集成 systemd。

**本文档用于**: GitHub Copilot, Cursor AI, 以及其他 AI 编程助手

## 架构概览

### 模块化设计（7 个独立模块）
```
src/
├── main.c         # 程序入口，信号处理 (SIGINT/SIGTERM/SIGUSR1)
├── common.c/h     # 工具函数：时间戳、字符串处理、环境变量
├── logger.c/h     # 5 级日志系统 (SILENT/ERROR/WARN/INFO/DEBUG)
├── config.c/h     # 配置管理：CLI + 环境变量 + 验证
├── icmp.c/h       # 原生 ICMP 实现 (raw socket, IPv4/IPv6)
├── systemd.c/h    # systemd 集成：sd_notify、watchdog、状态通知
└── monitor.c/h    # 监控循环：ping + 重试 + 失败统计 + 关机触发
```

**依赖关系**: common → logger → config/icmp/systemd → monitor → main

**关键特性**:
- 零第三方依赖（仅 C 标准库和 Linux 系统调用）
- 单一二进制 (39 KB)，内存占用 < 5 MB
- 需要 `CAP_NET_RAW` 权限（ICMP raw socket）
- 配置优先级：CLI 参数 > 环境变量 > 默认值

## 编码规范

### 命名约定（严格遵守）
```c
// 类型：小写 + 下划线 + _t 后缀
typedef struct {
    int value;
} config_t;

typedef enum {
    LOG_LEVEL_INFO = 2,
    LOG_LEVEL_DEBUG = 3
} log_level_t;

// 函数：module_action 格式
void logger_init(logger_t* logger, ...);
bool config_validate(const config_t* config, char* error_msg, size_t error_size);
ping_result_t icmp_pinger_ping(icmp_pinger_t* pinger, ...);

// 静态函数：static 关键字
static bool resolve_target(const char* target, ...);
static uint16_t calculate_checksum(const void* data, size_t len);

// 常量：大写 + 下划线
#define MAX_PATH_LENGTH 4096
#define PROGRAM_NAME "openups"

// 变量：小写 + 下划线
int consecutive_fails;
char error_msg[256];
```

### 代码风格
- **缩进**: 4 空格（不使用 tab）
- **大括号**: K&R 风格（函数定义另起一行，其他紧跟）
- **指针**: 星号靠近类型 (`int* ptr`, `const char* str`)
- **行长**: <= 100 字符
- **函数长度**: 尽量 <= 50 行

### 安全编码（10/10 安全评级要求）
```c
// ✅ 使用 snprintf（不使用 strcpy/strcat/sprintf）
snprintf(buffer, sizeof(buffer), "%s: %d", msg, value);

// ✅ 路径验证（防止遍历和注入）
static bool is_safe_path(const char* path) {
    return !(strstr(path, "..") || strstr(path, "//") || 
             strchr(path, ';') || strchr(path, '|') || 
             strchr(path, '&') || strchr(path, '`'));
}

// ✅ 边界检查
if (packet_size < 0 || packet_size > 65507) {
    snprintf(error_msg, error_size, "Packet size out of range");
    return false;
}

// ✅ 错误处理：bool 返回值 + error_msg
bool function_name(args, char* error_msg, size_t error_size) {
    if (error_condition) {
        snprintf(error_msg, error_size, "Error: %s", details);
        return false;
    }
    return true;
}

// ✅ restrict 关键字（C23）：优化指针别名
bool icmp_pinger_init(icmp_pinger_t* restrict pinger, 
                      char* restrict error_msg, size_t error_size);
```

## 关键系统

### 日志系统（自然语序，非 key=value）
```c
// 5 级日志（v1.1.0 统一设计）
typedef enum {
    LOG_LEVEL_SILENT = -1,  // 完全静默（适合 systemd）
    LOG_LEVEL_ERROR = 0,    // 仅错误
    LOG_LEVEL_WARN = 1,     // 警告 + 错误
    LOG_LEVEL_INFO = 2,     // 默认级别
    LOG_LEVEL_DEBUG = 3     // 详细日志 + 每次 ping 延迟
} log_level_t;

// 初始化（3 个参数）
logger_t logger;
logger_init(&logger, LOG_LEVEL_INFO, true, false);
//                   ^level          ^timestamp ^syslog

// printf 风格（编译时检查）- 使用自然语序
logger_info(&logger, "Starting monitor for target %s, checking every %ds", 
            config.target, config.interval_sec);
logger_debug(&logger, "Ping successful, latency: %.2fms", latency);
logger_warn(&logger, "Reached %d consecutive failures, threshold is %d",
            fails, threshold);
logger_error(&logger, "Failed to create socket: %s", strerror(errno));

// systemd journald 集成（避免双重时间戳）
Environment="OPENUPS_NO_TIMESTAMP=true"  // systemd 服务中设置
```

### 配置系统（3 层优先级）
```c
typedef struct {
    char target[256];
    int interval_sec;
    int fail_threshold;
    log_level_t log_level;      // v1.1.0: 统一日志级别
    bool enable_timestamp;      // 时间戳开关（systemd 场景下禁用）
    bool use_syslog;
    shutdown_mode_t shutdown_mode;  // immediate/delayed/log-only/custom
    bool dry_run;               // 默认 true（防止误操作）
    bool use_ipv6;
    // ... 更多字段见 config.h
} config_t;

// 初始化流程（main.c 标准模式）
config_t config;
config_init_default(&config);           // 1. 默认值
config_load_from_env(&config);          // 2. 环境变量
config_load_from_cmdline(&config, argc, argv);  // 3. CLI 参数（最高优先级）

char error_msg[256];
if (!config_validate(&config, error_msg, sizeof(error_msg))) {
    fprintf(stderr, "Configuration error: %s\n", error_msg);
    return 2;
}
```

### ICMP Ping 实现（原生 raw socket）
```c
// 初始化（需要 CAP_NET_RAW）
icmp_pinger_t pinger;
if (!icmp_pinger_init(&pinger, use_ipv6, error_msg, sizeof(error_msg))) {
    logger_error(&logger, error_msg);
    return false;
}

// 执行 ping（微秒级延迟测量）
ping_result_t result = icmp_pinger_ping(&pinger, "1.1.1.1", 
                                        2000, 56);  // timeout_ms, packet_size
if (result.success) {
    logger_debug(&logger, "Ping successful, latency: %.2fms", result.latency_ms);
} else {
    logger_warn(&logger, "Ping failed: %s", result.error_msg);
}

// 关键实现细节
// - IPv4: 手动计算 ICMP 校验和（calculate_checksum）
// - IPv6: 内核自动处理校验和
// - 编译时断言：static_assert(sizeof(struct icmphdr) >= 8, ...)
```

### systemd 集成（深度整合）
```c
// 初始化（检测 NOTIFY_SOCKET）
systemd_notifier_t systemd;
systemd_notifier_init(&systemd);

if (systemd.enabled) {
    systemd_notifier_ready(&systemd);  // 通知启动完成
    
    // 主循环中
    systemd_notifier_status(&systemd, "Monitoring target=1.1.1.1");
    systemd_notifier_watchdog(&systemd);  // 每秒发送心跳
}

// 工作原理
// - 通过 UNIX domain socket 发送通知到 $NOTIFY_SOCKET
// - 支持抽象命名空间（@ 前缀）
// - watchdog: 从 $WATCHDOG_USEC 读取超时时间
```

## 开发工作流

### 编译
```bash
# 完整编译（-O3 + LTO + native + 所有安全标志）
make

# 调试版本
make clean
make CC=gcc CFLAGS="-g -O0 -std=c2x -Wall -Wextra"

# 安装到系统
sudo make install  # → /usr/local/bin/openups + setcap cap_net_raw+ep
```

### 测试
```bash
# 基本测试（需要 root 或 CAP_NET_RAW）
sudo ./bin/openups --target 127.0.0.1 --interval 1 --threshold 3 --dry-run --log-level debug

# 授予 capability 后
sudo setcap cap_net_raw+ep ./bin/openups
./bin/openups --target 1.1.1.1 --interval 1 --threshold 3 --dry-run

# 自动化测试套件（10 个测试用例）
./test.sh
# 测试包括：编译检查、功能测试、输入验证、安全性测试、边界条件
```

### 调试
```bash
# GDB 调试
gdb --args ./bin/openups --target 127.0.0.1 --log-level debug

# 发送信号查看统计
kill -USR1 $(pidof openups)  # 输出成功率、延迟、运行时长

# journalctl 查看 systemd 日志
journalctl -u openups -f
```

## 常见任务模式

### 添加新配置项
1. 在 `config.h` 的 `config_t` 添加字段
2. `config_init_default()` 设置默认值
3. `config_load_from_env()` 添加环境变量 `OPENUPS_*`
4. `config_load_from_cmdline()` 添加 `--xxx` 选项
5. `config_validate()` 添加验证逻辑
6. `config_print_usage()` 添加帮助文本
7. 更新 README.md 配置表格

### 添加新日志级别或函数
1. `logger.h` 中 `log_level_t` 添加枚举值
2. `logger.c` 中 `log_level_to_string()` 添加字符串映射
3. `string_to_log_level()` 添加解析逻辑
4. 添加 `logger_xxx()` 函数（使用 `__attribute__((format(printf, 2, 3)))`）

### 修改 ICMP 行为
- 重点文件：`src/icmp.c`
- 关键函数：`icmp_pinger_ping()`, `calculate_checksum()`
- 注意：IPv4/IPv6 校验和处理不同，IPv6 需要 `IPV6_CHECKSUM` socket 选项

### systemd 服务配置
```ini
[Service]
# 环境变量（推荐方式）
Environment="OPENUPS_TARGET=192.168.1.1"
Environment="OPENUPS_INTERVAL=60"
Environment="OPENUPS_THRESHOLD=5"
Environment="OPENUPS_LOG_LEVEL=info"
Environment="OPENUPS_NO_TIMESTAMP=true"  # 避免双重时间戳
Environment="OPENUPS_DRY_RUN=false"

# 安全限制
CapabilityBoundingSet=CAP_NET_RAW
AmbientCapabilities=CAP_NET_RAW
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
PrivateTmp=true
MemoryMax=50M
```

## 性能和安全注意事项

### 性能优化技术
- **C23 标准**: 使用 `nullptr`, `static_assert`, `restrict` 关键字
- **编译优化**: `-O3 -flto -march=native -mtune=native`（Makefile 默认）
- **内存管理**: 关键路径使用栈上固定大小缓冲区（避免 malloc）
- **循环优化**: 监控循环 99% 时间在 `sleep()`，CPU 占用 < 1%

### 安全清单（必须保持 10/10）
- [x] Full RELRO (`-Wl,-z,relro,-z,now`)
- [x] PIE (`-fPIE -pie`)
- [x] Stack Canary (`-fstack-protector-strong`)
- [x] NX Stack (`-Wl,-z,noexecstack`)
- [x] FORTIFY_SOURCE=3 (`-D_FORTIFY_SOURCE=3`)
- [x] 所有字符串操作使用 `snprintf`（禁止 `strcpy/strcat/sprintf`）
- [x] 路径验证（`is_safe_path()`）
- [x] 命令注入防护（白名单 + 字符过滤）

### 编译器要求
- **最低要求**: GCC 14.0+ 或 Clang 15.0+（C23 支持）
- **推荐**: GCC 14.2+
- **检查**: `gcc --version` 和 `gcc -std=c2x -E -dM - < /dev/null | grep __STDC_VERSION__`

## 文档更新规则

修改代码后必须同步更新：
- `README.md` - 功能和配置表格
- `QUICKSTART.md` - 使用示例
- `TECHNICAL.md` - 架构变更和技术细节
- `CHANGELOG.md` - 版本记录
- `.github/copilot-instructions.md` - AI 上下文（如有重大变更）

## v1.1.0 关键变更（重要）

### 日志系统简化（2025-10-26）
- ❌ 移除 `-v/--verbose`, `-q/--quiet` 别名
- ✅ 统一使用 `--log-level` 参数（silent|error|warn|info|debug）
- ✅ 新增 `LOG_LEVEL_SILENT` 静默模式
- ✅ 移除 `config.verbose` 和 `logger.verbose` 字段

### systemd journald 深度集成
- ✅ 新增 `OPENUPS_NO_TIMESTAMP` 环境变量
- ✅ systemd 服务自动禁用程序时间戳（避免双重时间戳）
- ✅ 日志格式：`Oct 26 22:37:19 host openups[pid]: [LEVEL] message`

**迁移注意**: 旧代码使用 `--verbose` 需改为 `--log-level debug`，使用 `--quiet` 需改为 `--log-level error` 或 `--log-level silent`。

## 快速参考

```bash
# 最小化测试命令（干跑模式，调试日志）
sudo ./bin/openups --target 127.0.0.1 --interval 1 --threshold 2 --dry-run --log-level debug

# 生产环境配置（systemd）
Environment="OPENUPS_TARGET=192.168.1.1"
Environment="OPENUPS_INTERVAL=60"
Environment="OPENUPS_THRESHOLD=5"
Environment="OPENUPS_NO_TIMESTAMP=true"
Environment="OPENUPS_DRY_RUN=false"

# 编译 + 测试流程
make clean && make && ./test.sh

# 安全检查
checksec --file=./bin/openups  # 应显示 Full RELRO, PIE, Canary, NX
```
