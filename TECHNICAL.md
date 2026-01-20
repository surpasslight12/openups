# OpenUPS 技术文档

**C 标准**: C23 (C2x)

本文档整合了架构设计和开发指南，为开发者提供完整的技术参考。

---

## 📋 目录

- [架构设计](#架构设计)
- [模块详解](#模块详解)
- [开发规范](#开发规范)
- [性能优化](#性能优化)
- [安全设计](#安全设计)
- [运行参考](#运行参考)
- [常见问题](#常见问题)

---

### C23 特性使用

#### ✅ 指针与返回值检查（C 风格说明）
示例采用 C 语言惯用写法：使用 `NULL` 作为空指针检查；对关键 API 使用 C23 的 `[[nodiscard]]`
提示调用方检查返回值。

```c
static monitor_t* g_monitor = NULL;

if (monitor == NULL) {
    return false;
}

/* 示例：关键函数使用 [[nodiscard]] */
[[nodiscard]] bool config_validate(const config_t* config, char* error_msg, size_t error_size);
[[nodiscard]] int monitor_run(monitor_t* monitor);
```

#### ✅ restrict（优化指针别名）
```c
bool icmp_pinger_init(icmp_pinger_t* restrict pinger,
                      char* restrict error_msg, size_t error_size);
```

#### ✅ static_assert（编译时断言）
```c
static_assert(sizeof(sig_atomic_t) >= sizeof(int),
              "sig_atomic_t must be at least int size");
```

#### ✅ `stdckdint.h`（checked arithmetic）
用于时间换算、延迟/运行时长等场景的溢出安全计算（如秒 → 毫秒）。在编译器不支持 C23 头文件时，
项目会自动回退到等价的安全实现。

#### ✅ `CLOCK_MONOTONIC`（稳定的延迟/运行时长）
延迟测量与 uptime 统计使用单调时钟，避免系统时间调整（NTP/手动改时间）造成负数延迟或运行时长跳变。
├── README.md          # 项目说明
├── TECHNICAL.md       # 本文件
├── CONTRIBUTING.md    # 贡献指南
└── LICENSE            # MIT 许可证
```

### 依赖关系图

```
common.h (无依赖)
  ↑
logger.h (依赖 common.h)
  ↑
├─ config.h (依赖 logger.h, common.h)
├─ icmp.h (依赖 common.h)
├─ systemd.h (无额外依赖)
└─ monitor.h (依赖 config.h, logger.h, icmp.h, systemd.h)
  ↑
main.c (依赖 monitor.h, config.h, logger.h)
```

### 数据流

#### 启动流程
```
1. main() 解析 CLI 参数
2. config_load_from_env() 合并环境变量
3. config_validate() 验证配置
4. logger_init() 初始化日志
5. monitor_init() 初始化监控器（包括 ICMP pinger、systemd notifier）
6. systemd_notifier_ready() 通知 systemd
7. 进入主循环
```

#### 监控循环
```
1. icmp_pinger_ping() 执行 ICMP ping
2. handle_ping_success/failure() 处理结果
3. metrics_record_success/failure() 更新指标
4. 检查失败阈值 → trigger_shutdown()
5. 固定间隔休眠
6. 休眠期间每秒发送 watchdog 心跳
```

#### 关机流程
```
1. 连续失败达到阈值
2. trigger_shutdown() 被调用
3. 根据 shutdown_mode_t 执行：
   - IMMEDIATE: 立即关机
   - DELAYED: 延迟关机
   - LOG_ONLY: 仅记录日志
4. 记录日志（warn 级别）
5. 执行系统命令（如非 dry-run）

补充说明：关机命令通过 `fork()` + `execvp` 执行，不经过 shell。
```

---

## 模块详解

### 1. common 模块 (`common.c/h`)

**职责**：通用工具函数

**关键 API**：
- `get_timestamp_ms()` - 毫秒级时间戳
- `get_timestamp_str()` - 格式化时间字符串
- `get_env_or_default()` - 环境变量读取
- `get_env_bool()`/`get_env_int()` - 类型化环境变量
- `trim_whitespace()` - 字符串处理
- `str_equals()` - 字符串比较

**依赖**：无

---

### 2. logger 模块 (`logger.c/h`)

**职责**：自然语序日志系统

**关键 API**：
```c
typedef struct {
    log_level_t level;
    bool enable_timestamp;
} logger_t;

void logger_init(logger_t* logger, ...);
void logger_info(logger_t* logger, const char* fmt, ...)
    __attribute__((format(printf, 2, 3)));
```

**特性**：
- 输出到 stderr（通过 systemd 自动捕获到 journalctl）
- printf 风格的可变参数格式化
- 自然语序输出（不使用 key=value 格式）
- 可配置日志级别（SILENT/ERROR/WARN/INFO/DEBUG）
- 编译时格式检查（`__attribute__((format(printf, 2, 3)))`）

**日志格式**：`[TIMESTAMP] [LEVEL] natural language message`

**示例**：
- `[YYYY-MM-DD HH:MM:SS.mmm] [INFO] Starting OpenUPS monitor: target=127.0.0.1 interval=1s threshold=3 ipv6=false`
- `[YYYY-MM-DD HH:MM:SS.mmm] [DEBUG] Ping successful to 127.0.0.1, latency: 0.01ms`

**依赖**：common

---

### 3. config 模块 (`config.c/h`)

**职责**：配置解析和验证

**关键 API**：
```c
typedef struct {
    char target[256];
    int interval_sec;
    int fail_threshold;
    shutdown_mode_t shutdown_mode;
    bool dry_run;
    log_level_t log_level;
    // ... 更多配置项
} config_t;

void config_init_default(config_t* config);
void config_load_from_env(config_t* config);
bool config_load_from_cmdline(config_t* config, int argc, char** argv);
bool config_validate(const config_t* config, char* error_msg, size_t error_size);
```

**配置优先级**：CLI 参数 > 环境变量 > 默认值

**依赖**：logger, common

---

### 4. icmp 模块 (`icmp.c/h`)

**职责**：原生 ICMP ping 实现

**关键 API**：
```c
typedef struct {
    bool success;
    double latency_ms;
    char error_msg[256];
} ping_result_t;

bool icmp_pinger_init(icmp_pinger_t* pinger, bool use_ipv6, ...);
ping_result_t icmp_pinger_ping(icmp_pinger_t* pinger, const char* target,
                               int timeout_ms, int packet_size);
```

**实现细节**：
- **IPv4**：手动计算 ICMP 校验和
- **IPv6**：内核自动处理校验和
- 微秒级延迟测量
- 需要 `CAP_NET_RAW` 权限

**依赖**：common

---

### 5. systemd 模块 (`systemd.c/h`)

**职责**：systemd 集成

**关键 API**：
```c
typedef struct {
    bool enabled;
    char* notify_socket;
    int sockfd;
    uint64_t watchdog_usec;
} systemd_notifier_t;

bool systemd_notifier_ready(systemd_notifier_t* notifier);
bool systemd_notifier_status(systemd_notifier_t* notifier, const char* status);
bool systemd_notifier_watchdog(systemd_notifier_t* notifier);
```

**工作原理**：
1. 检查 `NOTIFY_SOCKET` 环境变量
2. 通过 UNIX domain socket 发送通知
3. 支持抽象命名空间（`@` 前缀）

**依赖**：无

---

### 6. monitor 模块 (`monitor.c/h`)

**职责**：监控循环和关机触发

**关键 API**：
```c
typedef struct {
    uint64_t total_pings;
    uint64_t successful_pings;
    uint64_t failed_pings;
    double min_latency;
    double max_latency;
    double total_latency;
} metrics_t;

bool monitor_init(monitor_t* monitor, config_t* config, logger_t* logger, ...);
int monitor_run(monitor_t* monitor);
void monitor_print_statistics(monitor_t* monitor);
```

**主循环伪代码**：
```c
while (!stop_flag) {
    1. 执行 ICMP ping (带重试)
    2. 记录成功/失败
    3. 检查失败阈值
    4. 触发关机（如需要）
    5. 固定间隔休眠
    6. 发送 watchdog 心跳
}
```

**依赖**：config, logger, icmp, systemd

---

### 7. main 模块 (`main.c`)

**职责**：程序入口

**流程**：
```c
int main(int argc, char** argv) {
    1. 初始化默认配置
    2. 从环境变量加载配置
    3. 从命令行加载配置
    4. 验证配置
    5. 初始化日志器
    6. 初始化监控器
    7. 运行主循环
    8. 清理资源
}
```

**依赖**：所有模块

---

## 开发规范

### 命名约定

```c
// 类型：小写 + 下划线 + _t
typedef struct {
    int value;
} config_t;

typedef enum {
    LOG_LEVEL_INFO = 2
} log_level_t;

// 函数：module_action
void logger_init(logger_t* logger, ...);
bool config_validate(const config_t* config, ...);

// 静态函数
static bool resolve_target(const char* target, ...);

// 常量：大写 + 下划线
#define MAX_PATH_LENGTH 4096
#define PROGRAM_NAME "openups"

// 变量：小写 + 下划线
int consecutive_fails;
char error_msg[256];
```

### 代码风格

```c
// 缩进：4 空格
void function() {
    if (condition) {
        // K&R 风格大括号
    }
}

// 函数定义：大括号另起一行
bool config_validate(const config_t* config)
{
    // ...
}

// 指针：星号靠近类型
int* ptr;
const char* str;

// 行长：<= 100 字符
```

### C23 特性使用

#### ✅ 空指针与返回值检查（C 风格示例）
示例采用 C 语言惯用写法：使用 `NULL` 作为空指针检查，并建议在可用的编译器上使用 `__attribute__((warn_unused_result))` 或等效机制提示检查返回值。

```c
static monitor_t* g_monitor = NULL;

if (monitor == NULL) {
    return false;
}

/* 若需强制检查返回值（GCC/Clang）可使用： */
bool config_validate(const config_t* config, char* error_msg, size_t error_size)
    __attribute__((warn_unused_result));

int monitor_run(monitor_t* monitor) __attribute__((warn_unused_result));
```

#### ✅ restrict（优化指针别名）
```c
bool icmp_pinger_init(icmp_pinger_t* restrict pinger,
                      char* restrict error_msg, size_t error_size);
```

#### ✅ static_assert（编译时断言）
```c
static_assert(sizeof(sig_atomic_t) >= sizeof(int),
              "sig_atomic_t must be at least int size");
static_assert(sizeof(struct icmphdr) >= 8,
              "icmphdr must be at least 8 bytes");
```

### 安全编码

#### 字符串操作
```c
// ✅ 始终使用 snprintf
snprintf(buffer, sizeof(buffer), "%s: %d", msg, value);

// ❌ 禁止使用
strcpy(dest, src);        // 不安全
strcat(dest, src);        // 不安全
sprintf(buffer, ...);     // 不安全
```

#### 路径验证
```c
// common.c
bool is_safe_path(const char* path) {
    return !(strstr(path, "..") || strstr(path, "//") ||
             strchr(path, ';') || strchr(path, '|') ||
             strchr(path, '&') || strchr(path, '`'));
}
```

#### 错误处理模式
```c
// 标准模式：bool 返回 + error_msg 参数
bool function_name(args, char* restrict error_msg, size_t error_size) {
    if (error_condition) {
        snprintf(error_msg, error_size, "Error: %s", details);
        return false;
    }
    return true;
}

// 调用方式
char error_msg[256];
if (!function_name(args, error_msg, sizeof(error_msg))) {
    logger_error(&logger, "%s", error_msg);
    return EXIT_FAILURE;
}
```

### 日志记录原则

```c
// ✅ 使用自然语序
logger_info(&logger, "Starting monitor for target %s, checking every %ds",
            target, interval);

// ❌ 避免 key=value 格式
logger_info(&logger, "target=%s interval=%d", target, interval);

// 日志级别使用
// SILENT: 完全静默
// ERROR:  仅致命错误
// WARN:   警告 + 错误
// INFO:   重要事件（默认）
// DEBUG:  详细调试（包括每次 ping）
```

---

## 性能优化

### 编译优化标志

```makefile
CFLAGS = -O3 -std=c2x -flto -march=native -mtune=native
         -fstack-protector-strong -fPIE -D_FORTIFY_SOURCE=3

LDFLAGS = -Wl,-z,relro,-z,now -Wl,-z,noexecstack -pie -flto
```

### 运行时性能

- **CPU 占用**: < 1%（主循环 99% 时间在 sleep）
- **内存占用**: < 5 MB
- **二进制大小**: 43 KB
- **启动时间**: < 10ms

### 内存使用

- **总体**：< 5 MB（主要是静态分配）
- **ICMP 缓冲区**：~4 KB（接收缓冲区）
- **配置结构**：< 2 KB

### 关键路径优化

```c
// 早期返回避免不必要计算
if (logger == NULL || logger->level < LOG_LEVEL_DEBUG) {
    return;  // 跳过格式化
}

// 栈内存避免 malloc
char buffer[256];  // 而非 malloc(256)

// restrict 关键字优化指针别名
void func(int* restrict a, int* restrict b) {
    // 编译器知道 a 和 b 不会重叠
}
```

---

## 安全设计

### 权限管理

- **最小权限**：仅需 `CAP_NET_RAW`
- **systemd 安全策略**：
  - `CapabilityBoundingSet=CAP_NET_RAW`
  - `NoNewPrivileges=true`
  - `ProtectSystem=strict`
  - `ProtectHome=true`

### 输入验证

- 配置参数范围检查
- 目标地址解析验证
- 命令行参数清理

### 编译时检查清单

- [x] Full RELRO (`readelf -d bin/openups | grep RELRO`)
- [x] PIE (`checksec --file=bin/openups`)
- [x] Stack Canary (`-fstack-protector-strong`)
- [x] NX Stack (`-Wl,-z,noexecstack`)
- [x] FORTIFY_SOURCE=3 (`-D_FORTIFY_SOURCE=3`)

### 代码审计清单

- [x] 所有字符串操作使用 `snprintf`
- [x] 路径验证（`is_safe_path()`）
- [x] 整数溢出检查（`ckd_mul`, `ckd_add`）
- [x] 边界检查（packet_size, timeout_ms）
- [x] 信号处理异步安全

---

## 运行参考

本节面向部署与运维：常用命令、systemd 配置、排错思路与已知限制。

### 常用命令

```bash
# 基本监控
./bin/openups --target 1.1.1.1 --interval 10 --threshold 5

# 生产启用实际关机（谨慎）
sudo ./bin/openups --target 192.168.1.1 --interval 5 --threshold 3 --dry-run=false

# 延迟关机
sudo ./bin/openups --target 8.8.8.8 --shutdown-mode delayed --delay 5 --dry-run=false

# 仅记录日志
sudo ./bin/openups --target 192.168.1.1 --shutdown-mode log-only

# IPv6
sudo ./bin/openups --target 2001:4860:4860::8888 --ipv6 --interval 5 --threshold 3
```

### systemd 部署（推荐）

```bash
make
sudo cp bin/openups /usr/local/bin/
sudo chmod 755 /usr/local/bin/openups
sudo setcap cap_net_raw+ep /usr/local/bin/openups

sudo cp systemd/openups.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now openups
sudo systemctl status openups
```

配置（推荐用环境变量覆盖）：

```bash
sudo systemctl edit openups
```

```ini
[Service]
Environment="OPENUPS_TARGET=8.8.8.8"
Environment="OPENUPS_INTERVAL=10"
Environment="OPENUPS_THRESHOLD=5"
Environment="OPENUPS_DRY_RUN=false"
Environment="OPENUPS_TIMESTAMP=false"
```

查看日志：

```bash
sudo journalctl -u openups -f
```

### 故障排查

1) `Operation not permitted`

原因：需要 root 权限或 `CAP_NET_RAW`。

```bash
sudo ./bin/openups --target 1.1.1.1

sudo setcap cap_net_raw+ep ./bin/openups
./bin/openups --target 1.1.1.1
```

2) systemd 启动失败

```bash
sudo journalctl -xe -u openups
sudo systemctl cat openups
```

3) 目标不可达 / DNS 问题

建议优先使用 IP；或先确认 DNS：

```bash
nslookup 目标主机
```

### 性能基准（示例）

| 场景 | CPU | 内存 | 网络 |
|------|-----|------|------|
| Idle (休眠中) | < 0.1% | 2.1 MB | 无 |
| 正常监控 (ping 间隔 10s) | 0.8% | 2.3 MB | 1 packet/10s |
| 高频监控 (ping 间隔 1s) | 2.1% | 2.4 MB | 1 packet/1s |
| 失败重试 (3 次重试) | 2.8% | 2.5 MB | 3 packets/cycle |

### 已知限制

- 仅支持 ICMP（不支持 TCP/UDP 探测）；某些网络可能过滤 ICMP。
- 目标地址：当前建议使用数字 IP；域名解析行为以实现与系统解析配置为准。
- Linux 专属；ICMP raw socket 需要 root 或 `CAP_NET_RAW`。
- 单进程单目标；多目标需要启动多个实例。



## 常见问题

### Q1: 为什么 max_retries=2 却尝试 3 次？
**A**: 初始尝试 + 重试次数 = 总尝试次数
```c
for (int attempt = 0; attempt <= max_retries; attempt++) {
    // attempt 0: 初始尝试
    // attempt 1: 第 1 次重试
    // attempt 2: 第 2 次重试
}
```

### Q2: LOG_ONLY 模式的行为？
**A**: 达到阈值时记录日志，重置计数器，继续监控
```c
if (monitor->config->shutdown_mode == SHUTDOWN_MODE_LOG_ONLY) {
    trigger_shutdown(monitor);
    monitor->consecutive_fails = 0;  // 重置继续监控
}
```

### Q3: 为什么编译需要 GCC 14+？
**A**: C23 特性支持（static_assert, restrict 等）
```bash
# 检查编译器版本
gcc --version

# 检查 C23 支持
gcc -std=c2x -E -dM - < /dev/null | grep __STDC_VERSION__
```

### Q4: 如何调试 ICMP 权限问题？
```bash
# 方法 1: 使用 sudo
sudo ./bin/openups ...

# 方法 2: 设置 capability
sudo setcap cap_net_raw+ep ./bin/openups
./bin/openups ...

# 方法 3: 检查权限
getcap ./bin/openups
```

### Q5: systemd watchdog 如何工作？
```c
// 从环境变量读取超时时间
const char* watchdog_str = getenv("WATCHDOG_USEC");
notifier->watchdog_usec = strtoull(watchdog_str, NULL, 10);

// 每秒发送心跳（在 sleep_with_stop 循环中）
systemd_notifier_watchdog(monitor->systemd);
```

### Q6: 为什么需要 EINTR 重试？
**A**: 系统调用可能被信号中断（如 SIGWINCH）
```c
// systemd.c 中的正确实现
ssize_t sent;
do {
    sent = sendto(sockfd, message, len, 0, addr, addrlen);
} while (sent < 0 && errno == EINTR);
```

### Q7: 如何添加新的配置项？
```
1. config.h: 添加字段到 config_t
2. config.c: config_init_default() 设置默认值
3. config.c: config_load_from_env() 添加 OPENUPS_* 环境变量
4. config.c: config_load_from_cmdline() 添加 --xxx 参数
5. config.c: config_validate() 添加验证逻辑
6. config.c: config_print_usage() 更新帮助信息
7. README.md: 更新配置表格
```

---

## 扩展指南

### 添加新的配置项

1. 在 `config.h` 的 `config_t` 结构体添加字段
2. 在 `config_init_default()` 设置默认值
3. 在 `config_load_from_env()` 添加环境变量读取
4. 在 `config_load_from_cmdline()` 添加 CLI 解析
5. 在 `config_validate()` 添加验证逻辑
6. 在 `config_print_usage()` 添加帮助文本

### 添加新的监控指标

1. 在 `metrics_t` 结构体添加字段
2. 在 `metrics_init()` 初始化
3. 在 `metrics_record_success/failure()` 更新字段
4. 在 `monitor_print_statistics()` 输出新指标

### 添加新的日志级别

1. 在 `log_level_t` 枚举添加新级别
2. 在 `log_level_to_string()` 添加字符串映射
3. 在 `string_to_log_level()` 添加解析逻辑
4. 添加对应的 `logger_xxx()` 函数

---

## 开发工作流

### 编译和测试

```bash
# 清理编译
make clean && make

# 基本测试
./bin/openups --target 127.0.0.1 --interval 1 --threshold 2 --dry-run

# 运行测试套件
./test.sh
```

### 调试技巧

```bash
# GDB 调试
gdb --args ./bin/openups --target 127.0.0.1 --log-level debug

# 发送信号查看统计
kill -USR1 $(pidof openups)

# journalctl 查看 systemd 日志
journalctl -u openups -f
```

### 代码质量检查

```bash
# 检查二进制安全特性
checksec --file=./bin/openups

# 预期输出：
# RELRO:    Full RELRO
# Stack:    Canary found
# NX:       NX enabled
# PIE:      PIE enabled
# FORTIFY:  Enabled
```

---

**维护**: OpenUPS 项目团队
