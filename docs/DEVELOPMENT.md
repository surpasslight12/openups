# OpenUPS 开发指南

**版本**: v1.1.0  
**更新**: 2025-11-01

本文档整合了代码审查、运行时分析和开发规范，为开发者提供完整的技术参考。

---

## 📋 目录

- [快速参考](#快速参考)
- [代码质量评估](#代码质量评估)
- [运行时行为](#运行时行为)
- [开发规范](#开发规范)
- [常见问题](#常见问题)

---

## 快速参考

### 编译和测试
```bash
# 清理编译
make clean && make

# 基本测试
./bin/openups --target 127.0.0.1 --interval 1 --threshold 2 --dry-run

# 运行测试套件
./test.sh
```

### 代码质量指标
| 指标 | 数值 | 评级 |
|------|------|------|
| 编译警告 | 0 | ⭐⭐⭐⭐⭐ |
| 代码行数 | ~1,700 | ⭐⭐⭐⭐⭐ |
| 二进制大小 | 39 KB | ⭐⭐⭐⭐⭐ |
| 内存占用 | < 5 MB | ⭐⭐⭐⭐⭐ |
| 安全评分 | 10/10 | ⭐⭐⭐⭐⭐ |

### 模块依赖图
```
common → logger → config/icmp/systemd → monitor → main
```

---

## 代码质量评估

### C23 特性使用

#### ✅ nullptr（完美使用）
```c
static monitor_t* g_monitor = nullptr;  // 替代 NULL

if (monitor == nullptr) {
    return false;
}
```

#### ✅ [[nodiscard]]（完美使用）
```c
[[nodiscard]] bool config_validate(const config_t* config, ...);
[[nodiscard]] int monitor_run(monitor_t* monitor);
```

#### ✅ restrict（完美使用）
```c
bool icmp_pinger_init(icmp_pinger_t* restrict pinger, 
                      char* restrict error_msg, size_t error_size);
```

#### ✅ static_assert（完美使用）
```c
static_assert(sizeof(sig_atomic_t) >= sizeof(int), 
              "sig_atomic_t must be at least int size");
static_assert(sizeof(struct icmphdr) >= 8, 
              "icmphdr must be at least 8 bytes");
```

### 安全特性

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

#### 整数溢出保护
```c
#include <stdckdint.h>

// 安全的乘法
if (ckd_mul(&result, a, b)) {
    // 溢出处理
}
```

---

## 运行时行为

### 程序生命周期

```
1. 启动阶段
   ├─ 配置初始化（默认值 → 环境变量 → CLI 参数）
   ├─ 配置验证
   ├─ 日志器初始化
   └─ 监控器初始化（ICMP socket + systemd + 信号处理）

2. 监控循环
   ├─ 执行 ping（带重试：max_retries=2 表示最多 3 次）
   ├─ 成功：重置失败计数，更新统计
   ├─ 失败：累加计数
   └─ 达到阈值：触发关机（LOG_ONLY 模式除外）

3. 优雅退出
   ├─ 处理 SIGINT/SIGTERM 信号
   ├─ 打印统计信息
   └─ 清理资源
```

### 重要状态机

#### 失败计数状态机
```
[consecutive_fails = 0] ──失败──> [consecutive_fails++]
         ↑                              ↓
         └───成功───────────── [>= threshold?]
                                        ↓
                              是 → 触发关机/重置（LOG_ONLY）
                              否 → 继续监控
```

#### 信号处理（异步安全）
```c
// signal_handler() - 仅修改 sig_atomic_t
static void signal_handler(int signum) {
    if (g_monitor) {
        if (signum == SIGINT || signum == SIGTERM) {
            g_monitor->stop_flag = 1;  // ✅ 异步安全
        }
    }
    // ❌ 不能调用 logger_info, systemd_notifier_stopping
}

// monitor_run() - 主循环检查标志
while (!monitor->stop_flag) {
    // ... 监控逻辑
}

// 循环外安全调用
if (monitor->stop_flag) {
    logger_info(logger, "Stopping gracefully...");  // ✅ 安全
}
```

### 实际测试结果

#### 正常运行（IPv4）
```
[INFO] Starting OpenUPS monitor for target 127.0.0.1, checking every 1 seconds, 
       shutdown after 2 consecutive failures (IPv4)
[INFO] Statistics: 6 total pings, 6 successful, 0 failed (100.00% success rate), 
       latency min 0.01ms / max 0.02ms / avg 0.02ms, uptime 6s
[INFO] OpenUPS monitor stopped
```

#### 失败触发（dry-run）
```
[WARN] Ping failed to 192.0.2.1: Timeout (consecutive failures: 2)
[WARN] Shutdown threshold reached, mode is immediate (dry-run enabled)
[INFO] [DRY-RUN] Would trigger shutdown in immediate mode
[INFO] Shutdown triggered, exiting monitor loop
```

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

### 错误处理模式

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

### 内存管理

```c
// 栈内存：小缓冲区
char buffer[256];
uint8_t packet[1024];

// 堆内存：大对象或动态大小
monitor->systemd = malloc(sizeof(systemd_notifier_t));
if (monitor->systemd == nullptr) {
    // 错误处理
    return false;
}

// 释放时置空
free(monitor->systemd);
monitor->systemd = nullptr;  // 防止悬空指针
```

---

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
**A**: C23 特性支持（nullptr, [[nodiscard]], static_assert）
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
notifier->watchdog_usec = strtoull(watchdog_str, nullptr, 10);

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
- **二进制大小**: 39 KB
- **启动时间**: < 10ms

### 关键路径优化
```c
// 早期返回避免不必要计算
if (logger == nullptr || logger->level < LOG_LEVEL_DEBUG) {
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

## 安全检查清单

### 编译时检查
- [x] Full RELRO (`readelf -d bin/openups | grep RELRO`)
- [x] PIE (`checksec --file=bin/openups`)
- [x] Stack Canary (`-fstack-protector-strong`)
- [x] NX Stack (`-Wl,-z,noexecstack`)
- [x] FORTIFY_SOURCE=3 (`-D_FORTIFY_SOURCE=3`)

### 代码审计
- [x] 所有字符串操作使用 `snprintf`
- [x] 路径验证（`is_safe_path()`）
- [x] 整数溢出检查（`ckd_mul`, `ckd_add`）
- [x] 边界检查（packet_size, timeout_ms）
- [x] 信号处理异步安全

### 运行时验证
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

## 贡献指南

### 提交代码前检查
```bash
# 1. 编译通过（0 警告）
make clean && make

# 2. 运行测试
./test.sh

# 3. 检查格式
# 缩进 4 空格，行长 <= 100

# 4. 更新文档
# 同步更新相关 Markdown 文件
```

### Pull Request 要求
1. **标题**: 清晰描述变更（如 "fix: 修复 ICMP 校验和计算错误"）
2. **描述**: 说明问题和解决方案
3. **测试**: 提供测试步骤和结果
4. **文档**: 更新相关文档

### 代码审查重点
- 内存安全（无泄漏、无悬空指针）
- 错误处理完整性
- 日志记录清晰性
- C23 特性正确使用

---

**维护**: OpenUPS 项目团队  
**最后更新**: 2025-11-01
