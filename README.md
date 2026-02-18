# OpenUPS - Network Monitor with Auto-Shutdown

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C23](https://img.shields.io/badge/C-23-blue.svg)](https://en.wikipedia.org/wiki/C23_(C_standard_revision))
[![systemd](https://img.shields.io/badge/systemd-integrated-green.svg)](https://systemd.io/)
[![Security](https://img.shields.io/badge/security-10%2F10-brightgreen.svg)](#-安全特性)
[![Version](https://img.shields.io/badge/version-1.0.0-blue.svg)](src/main.c)

**OpenUPS** 是一个**轻量级、高性能、高安全**的 Linux 网络监控工具，通过周期性 ICMP ping 检测网络可达性，并在连续失败达到阈值后自动执行关机或自定义脚本。

## ✨ 特性

### 核心功能
- **原生 ICMP 实现**：使用 raw socket，无需依赖系统 `ping` 命令
- **IPv4/IPv6 双栈支持**：同时支持 IPv4 和 IPv6 网络
- **智能重试机制**：可配置的重试次数
- **灵活的关机策略**：immediate、delayed、log-only 三种模式

### 性能优势
- **C23 标准**：使用最新 C 语言标准（`static_assert`、`restrict`、`stdckdint.h`、`ckd_add/ckd_mul`）
- **极致性能**：-O3 + LTO + CPU 原生优化，约 55 KB 二进制，< 5 MB 内存，< 1% CPU
- **统一上下文架构**：单参数传递，CPU 缓存友好的内存布局
- **ICMP 热路径优化**：payload 预填充与缓冲区复用

### 系统集成
- **systemd 深度集成**：支持 `sd_notify`、watchdog、状态通知
- **安全加固**：10/10 安全评级，Full RELRO + PIE + Stack Canary + FORTIFY_SOURCE=3

### 可靠性
- **信号处理**：优雅处理 SIGINT/SIGTERM/SIGUSR1
- **指标统计**：实时监控成功率、延迟、运行时长
- **dry-run 模式**：默认启用，防止误操作

## 🏗️ 架构（v1.0.0）

```
src/
└── main.c              # 单文件程序（包含全部功能模块）
```

所有模块代码整合在单一源文件中，按逻辑分区组织：
- **通用工具**：时间戳、环境变量读取、路径安全检查
- **日志系统**：5 级日志（SILENT/ERROR/WARN/INFO/DEBUG）
- **指标统计**：ping 成功率、延迟、运行时长
- **配置管理**：CLI 参数、环境变量、验证
- **ICMP 实现**：原生 raw socket（IPv4/IPv6）
- **关机触发**：fork/execvp 执行
- **systemd 集成**：sd_notify 协议、watchdog 心跳
- **上下文管理**：统一上下文、信号处理、监控循环

**设计原则**：
- ✅ 单文件架构，简化构建和部署
- ✅ 统一上下文架构（`openups_ctx_t`）
- ✅ 零第三方依赖（仅 C 标准库和 Linux 系统调用）
- ✅ 内置 systemd 集成（`sd_notify`、watchdog）

补充：当前版本仅提供可执行程序，不对外提供稳定的 C 库 API。

### 统一上下文架构（openups_ctx_t）

```c
typedef struct openups_context {
    /* === 热路径数据（频繁访问，CPU 缓存友好）=== */
    volatile sig_atomic_t stop_flag;        /* offset 0：信号安全 */
    volatile sig_atomic_t print_stats_flag; /* offset 4 */
    int consecutive_fails;                  /* offset 8：失败计数 */

    /* === systemd 集成（内置）=== */
    bool systemd_enabled;
    uint64_t watchdog_interval_ms;
    uint64_t last_ping_time_ms;
    uint64_t start_time_ms;

    /* === 核心组件（值类型，避免指针跳转）=== */
    config_t config;
    logger_t logger;
    metrics_t metrics;
    icmp_pinger_t pinger;
    systemd_notifier_t systemd;
} openups_ctx_t;
```

**优势**：
1. **单参数传递**：所有函数只需 `openups_ctx_t* ctx`（消除多指针传递）
2. **内存局部性**：所有组件内嵌（减少指针跳转，提升缓存命中率）
3. **热数据优先**：信号标志和失败计数放在结构体前部（64 字节内）
4. **初始化集中**：`openups_ctx_init()` 自动处理所有组件初始化

### 数据流

#### 启动流程
```
1. main() 调用 openups_ctx_init()
   ├─ config_init_default()          # 默认配置
   ├─ config_load_from_env()         # 环境变量
   ├─ config_load_from_cmdline()     # CLI 参数（最高优先级）
   ├─ config_validate()              # 验证配置
   ├─ logger_init()                  # 初始化日志
   ├─ icmp_pinger_init()             # 初始化 ICMP pinger
   ├─ systemd_notifier_init()        # 初始化 systemd 集成
   └─ metrics_init()                 # 初始化指标
2. main() 调用 openups_ctx_run()
   ├─ setup_signal_handlers()        # 设置信号处理
   ├─ systemd_notifier_ready()       # 通知 systemd
   └─ 进入主循环
```

#### 监控循环
```
while (!ctx->stop_flag) {
    1. run_iteration(ctx)
       ├─ openups_ctx_ping_once()      # 执行 ping（带重试）
       ├─ handle_ping_success/failure() # 处理结果
       ├─ metrics_record_*()            # 更新指标
       └─ trigger_shutdown()            # 检查失败阈值
    2. openups_ctx_sleep_interruptible() # 可中断休眠
       ├─ 分块休眠（每块 ≤ watchdog_interval_ms）
       ├─ systemd_notifier_watchdog()   # 周期性心跳
       └─ 检查 ctx->stop_flag
}
```

#### 关机流程
```
1. 连续失败达到阈值
2. trigger_shutdown() 被调用
3. 根据 shutdown_mode_t 执行：
   - IMMEDIATE: 立即关机
   - DELAYED: 延迟关机
   - LOG_ONLY: 仅记录日志
4. 执行系统命令（如非 dry-run），通过 fork() + execvp，不经过 shell
```

## 🚀 快速开始

面向首次使用者的最小流程：编译 → 查看帮助 → dry-run 验证 →（可选）安装为 systemd 服务。

### 前置要求

```bash
# Ubuntu/Debian
sudo apt install build-essential

# CentOS/RHEL
sudo yum groupinstall "Development Tools"
```

编译器要求：GCC 14.0+ 或 Clang 15.0+（C23 支持）。

### 编译

```bash
# 使用 Makefile
make

# 编译完成后，二进制文件位于 bin/openups
```

### 查看帮助

```bash
./bin/openups --help
```

### 测试运行

```bash
# 测试 ICMP ping（需要 root 或 CAP_NET_RAW）
sudo ./bin/openups --target 1.1.1.1 --interval 1 --threshold 3 --dry-run --log-level debug

# 或授予 capability
sudo setcap cap_net_raw+ep ./bin/openups
./bin/openups --target 1.1.1.1 --interval 1 --threshold 3 --dry-run
```

### 安装为系统服务

```bash
# 一键编译 + 安装二进制 + 安装 systemd service
sudo make install

# 启用并启动
sudo systemctl enable --now openups

# 查看帮助
openups --help
```

### 配置 systemd 服务（推荐用环境变量）

```bash
sudo systemctl edit openups
```

示例配置：

```ini
[Service]
Environment="OPENUPS_TARGET=8.8.8.8"
Environment="OPENUPS_INTERVAL=10"
Environment="OPENUPS_THRESHOLD=5"
Environment="OPENUPS_TIMEOUT=2000"
Environment="OPENUPS_MAX_RETRIES=2"
Environment="OPENUPS_SHUTDOWN_MODE=immediate"
Environment="OPENUPS_DRY_RUN=false"
Environment="OPENUPS_TIMESTAMP=false"

; 权限说明（默认 unit）：
; - service 以 root 运行，但 CapabilityBoundingSet 仅保留 CAP_NET_RAW + CAP_SYS_BOOT
; - CAP_SYS_BOOT 用于执行 shutdown/poweroff 命令
; - 仓库默认 OPENUPS_DRY_RUN=true（安全默认值）
; - 如需 systemd watchdog，请同时设置 WatchdogSec=30 并启用 OPENUPS_WATCHDOG=true
```

应用并启动：

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now openups
sudo systemctl status openups
sudo journalctl -u openups -f
```

## 📖 常用场景

### 基本监控

```bash
# 监控 Cloudflare DNS，10 秒间隔，5 次失败触发
./bin/openups --target 1.1.1.1 --interval 10 --threshold 5
```

### 延迟关机模式

```bash
# 失败后延迟 5 分钟关机
sudo ./bin/openups --target 8.8.8.8 --shutdown-mode delayed --delay 5 --dry-run=false
```

### 仅记录日志（不关机）

```bash
# 仅记录失败，持续监控
sudo ./bin/openups --target 192.168.1.1 --shutdown-mode log-only
```

### 实际关机模式

```bash
# 生产模式（真实关机）
sudo ./bin/openups --target 192.168.1.1 --interval 5 --threshold 3 --dry-run=false
```

### IPv6 监控

```bash
./bin/openups --target 2606:4700:4700::1111 --ipv6 --interval 10
```

> 注意：OpenUPS 不做 DNS 解析，`--target` 仅支持 IP 字面量（IPv4/IPv6）。

补充：默认不会直接调用内核 `reboot()` 关机接口；会通过 `fork()` + `exec*()` 调用
`systemctl poweroff`（systemd 场景）或 `/sbin/shutdown`（非 systemd 场景）。

## ⚙️ 常用参数

完整参数以 `./bin/openups --help` 为准。

### 默认值一览

| 参数 | CLI 选项 | 环境变量 | 默认值 | 说明 |
|------|----------|----------|--------|------|
| 监控目标 | `-t, --target` | `OPENUPS_TARGET` | `1.1.1.1` | 目标 IP 字面量（不支持域名） |
| 检测间隔 | `-i, --interval` | `OPENUPS_INTERVAL` | `10`（秒） | 两次 ping 之间的间隔 |
| 失败阈值 | `-n, --threshold` | `OPENUPS_THRESHOLD` | `5` | 连续失败多少次触发关机 |
| 超时时间 | `-w, --timeout` | `OPENUPS_TIMEOUT` | `2000`（ms） | 单次 ping 等待回包的超时 |
| 载荷大小 | `-s, --payload-size` | `OPENUPS_PAYLOAD_SIZE` | `56`（字节） | ICMP 数据载荷长度 |
| 重试次数 | `-r, --retries` | `OPENUPS_RETRIES` | `2` | 每次 ping 的重试次数（总尝试 = 重试 + 1） |
| IPv6 模式 | `-6, --ipv6` | `OPENUPS_IPV6` | `false` | 启用 IPv6（目标须为 IPv6 字面量） |
| 关机模式 | `-S, --shutdown-mode` | `OPENUPS_SHUTDOWN_MODE` | `immediate` | `immediate` / `delayed` / `log-only` |
| 延迟时长 | `-D, --delay` | `OPENUPS_DELAY_MINUTES` | `1`（分钟） | `delayed` 模式下的关机延迟 |
| 演习模式 | `-d, --dry-run` | `OPENUPS_DRY_RUN` | `true` | 不执行实际关机（安全默认值） |
| 日志级别 | `-L, --log-level` | `OPENUPS_LOG_LEVEL` | `info` | `silent` / `error` / `warn` / `info` / `debug` |
| 时间戳 | `-T, --timestamp` | `OPENUPS_TIMESTAMP` | `true` | 日志前缀时间戳（systemd 下建议 false） |
| systemd | `-M, --systemd` | `OPENUPS_SYSTEMD` | `true` | 启用 sd_notify 集成 |
| watchdog | `-W, --watchdog` | `OPENUPS_WATCHDOG` | `true` | 启用 systemd watchdog 心跳 |

## 🔒 安全特性

### 权限管理

**方式 1：使用 capability（推荐）**

```bash
sudo setcap cap_net_raw+ep ./bin/openups
./bin/openups --target 1.1.1.1
```

**方式 2：使用 root 权限**

```bash
sudo ./bin/openups --target 1.1.1.1
```

**systemd 安全策略**：
- `CapabilityBoundingSet=CAP_NET_RAW CAP_SYS_BOOT`
- `AmbientCapabilities=CAP_NET_RAW`
- `NoNewPrivileges=true`
- `ProtectSystem=strict`
- `ProtectHome=true`
- `SystemCallFilter=@system-service @network-io @reboot`（`@reboot` 用于执行 shutdown 命令）

### 输入验证

- 配置参数范围检查
- 目标地址解析验证（`inet_pton()`，仅 IP 字面量）
- 路径验证（`is_safe_path()` 防止遍历和注入）
- 整数溢出检查（`ckd_mul`, `ckd_add`）

### 编译时安全清单

- [x] Full RELRO (`-Wl,-z,relro,-z,now`)
- [x] PIE (`-fPIE -pie`)
- [x] Stack Canary (`-fstack-protector-strong`)
- [x] NX Stack (`-Wl,-z,noexecstack`)
- [x] FORTIFY_SOURCE=3 (`-D_FORTIFY_SOURCE=3`)
- [x] 所有字符串操作使用 `snprintf`（禁止 `strcpy/strcat/sprintf`）
- [x] 信号处理异步安全

```bash
# 验证安全特性
checksec --file=./bin/openups
```

## 📊 监控与调试

### 查看实时日志

```bash
# 发送 SIGUSR1 查看统计信息
kill -USR1 $(pidof openups)
```

### 调试模式

```bash
# 详细日志输出（包含每次 ping 的延迟）
./bin/openups --log-level debug --target 127.0.0.1
```

### 故障排查

1) `Operation not permitted` — 需要 root 权限或 `CAP_NET_RAW`：
```bash
sudo setcap cap_net_raw+ep ./bin/openups
```

2) systemd 启动失败：
```bash
sudo journalctl -xe -u openups
sudo systemctl cat openups
```

## 🧪 测试

运行自动化测试套件：

```bash
./test.sh
```

灰度验证（可选，需要 root 或 CAP_NET_RAW）：

```bash
# 进程级灰度验证
./test.sh --gray

# systemd 灰度验证（需要 systemd + root）
./test.sh --gray-systemd

# 运行全部测试（基础 + 灰度 + systemd 灰度）
./test.sh --all
```

## 🔧 技术细节

### C23 特性

| 特性 | 用途 |
|------|------|
| `restrict` | 优化指针别名，提升编译器优化能力 |
| `static_assert` | 编译时断言（如 `sizeof(sig_atomic_t) >= sizeof(int)`） |
| `stdckdint.h` / `ckd_add` / `ckd_mul` | 溢出安全的整数运算（秒→毫秒等时间换算）；老编译器自动回退到等价实现 |
| `CLOCK_MONOTONIC` | 延迟测量与 uptime 统计使用单调时钟，避免系统时间调整影响 |

编译器不支持 `stdckdint.h` 时，项目会自动回退到等价的安全实现。

### 模块概览

**依赖关系**：`通用工具 → 日志 → 指标 → 配置 → ICMP → 关机 → systemd → 上下文 → main()`

| 模块 | 关键 API |
|------|----------|
| 通用工具 | `get_monotonic_ms()`, `get_timestamp_str()`, `get_env_int()`, `get_env_bool()`, `is_safe_path()` |
| 日志 | `logger_init()`, `logger_info/warn/error/debug()` — 自然语序输出，编译时格式检查 |
| 配置 | `config_init_default()`, `config_load_from_env()`, `config_load_from_cmdline()`, `config_validate()` |
| ICMP | `icmp_pinger_init()`, `icmp_pinger_ping_ex()` → `ping_result_t { success, latency_ms, error_msg }` |
| systemd | `systemd_notifier_ready()`, `systemd_notifier_status()`, `systemd_notifier_watchdog()` |
| 上下文 | `openups_ctx_init()`, `openups_ctx_run()`, `openups_ctx_destroy()` |

**ICMP 实现细节**：
- IPv4：手动计算校验和（可选 AVX2 加速，运行时自动分发）
- IPv6：校验和由内核处理，尽力设置 `IPV6_CHECKSUM`
- raw socket 设为 non-blocking，等待回包使用 `poll()`，处理 `EINTR/EAGAIN`
- 需要 `CAP_NET_RAW` 权限

**配置优先级**：CLI 参数 > 环境变量 > 默认值

### 性能优化

**编译优化标志**：
```makefile
CFLAGS = -O3 -std=c23 -flto=auto -march=native -mtune=native
         -fstack-protector-strong -fPIE -D_FORTIFY_SOURCE=3
         -ffunction-sections -fdata-sections -fno-plt

LDFLAGS = -Wl,-z,relro,-z,now -Wl,-z,noexecstack -pie -flto=auto
          -Wl,--gc-sections -Wl,--as-needed -Wl,-O2
```

**运行时性能**：

| 场景 | CPU | 内存 | 网络 |
|------|-----|------|------|
| Idle (休眠中) | < 0.1% | 2.1 MB | 无 |
| 正常监控 (10s 间隔) | 0.8% | 2.3 MB | 1 packet/10s |
| 高频监控 (1s 间隔) | 2.1% | 2.4 MB | 1 packet/1s |
| 失败重试 (3 次) | 2.8% | 2.5 MB | 3 packets/cycle |

**关键路径优化**：
- 早期返回避免不必要计算（如日志级别判断）
- 栈内存避免 malloc
- `restrict` 关键字优化指针别名
- systemd 状态更新降频与可用状态缓存
- ICMP payload 预填充，仅清零 ICMP 头部

### 安全设计

- **最小权限**：仅需 `CAP_NET_RAW`
- **关机命令**：通过 `fork()` + `execvp` 执行，不经过 shell
- **路径验证**：`is_safe_path()` 过滤 `..`、`//`、`;`、`|`、`&`、`` ` `` 等字符
- **错误处理**：统一 `bool` 返回 + `error_msg` 参数模式
- **信号安全**：使用 `volatile sig_atomic_t` 标志

## 📝 开发规范

### 命名约定

```c
// 类型：小写 + 下划线 + _t
typedef struct { int value; } config_t;
typedef enum { LOG_LEVEL_INFO = 2 } log_level_t;

// 函数：module_action
void logger_init(logger_t* logger, ...);
bool config_validate(const config_t* config, ...);

// 静态函数：static 关键字
static bool resolve_target(const char* target, ...);

// 常量：大写 + 下划线
#define MAX_PATH_LENGTH 4096

// 变量：小写 + 下划线
int consecutive_fails;
```

### 代码风格

- **缩进**：4 空格（不使用 tab）
- **大括号**：K&R 风格（函数定义另起一行，其他紧跟）
- **指针**：星号靠近类型（`int* ptr`, `const char* str`）
- **行长**：<= 100 字符
- **注释分层**：源码中只保留用途和关键参数说明；设计动机等长文本放在文档中

### 安全编码

```c
// ✅ 始终使用 snprintf
snprintf(buffer, sizeof(buffer), "%s: %d", msg, value);

// ✅ 路径验证
bool is_safe_path(const char* path) {
    return !(strstr(path, "..") || strstr(path, "//") ||
             strchr(path, ';') || strchr(path, '|') ||
             strchr(path, '&') || strchr(path, '`'));
}

// ✅ 标准错误处理模式
bool function_name(args, char* restrict error_msg, size_t error_size) {
    if (error_condition) {
        snprintf(error_msg, error_size, "Error: %s", details);
        return false;
    }
    return true;
}
```

### 日志原则

```c
// ✅ 使用自然语序
logger_info(&logger, "Starting monitor for target %s, checking every %ds",
            target, interval);

// ❌ 避免 key=value 格式
logger_info(&logger, "target=%s interval=%d", target, interval);
```

日志级别：SILENT（完全静默） / ERROR（仅致命错误） / WARN（警告+错误） / INFO（默认） / DEBUG（详细调试）

## 🛠️ 开发工作流

### 编译和测试

```bash
# 清理编译
make clean && make

# 调试版本
make clean && make CC=gcc CFLAGS="-g -O0 -std=c23 -Wall -Wextra"

# 基本测试
./bin/openups --target 127.0.0.1 --interval 1 --threshold 2 --dry-run

# 运行测试套件
./test.sh

# 安装到系统（二进制 + service 文件 + setcap）
sudo make install

# 卸载（二进制 + service 文件）
sudo make uninstall

# GDB 调试
gdb --args ./bin/openups --target 127.0.0.1 --log-level debug

# 查看 systemd 日志
journalctl -u openups -f
```

### 扩展指南

**添加新配置项**：
1. `src/main.c`: 在 `config_t` 添加字段
2. `config_init_default()` 设置默认值
3. `config_load_from_env()` 添加 `OPENUPS_*` 环境变量
4. `config_load_from_cmdline()` 添加 `--xxx` 参数
5. `config_validate()` 添加验证逻辑
6. `config_print_usage()` 更新帮助信息
7. 更新本文档配置表格

**添加新监控指标**：
1. 在 `metrics_t` 添加字段 → `metrics_init()` 初始化
2. 在 `metrics_record_success/failure()` 更新
3. 在 `openups_ctx_print_stats()` 输出

**添加新日志级别**：
1. `log_level_t` 添加枚举值
2. `log_level_to_string()` / `string_to_log_level()` 添加映射
3. 添加 `logger_xxx()` 函数（使用 `__attribute__((format(printf, 2, 3)))`）

## ❓ 常见问题

**Q: 为什么 max_retries=2 却尝试 3 次？**
初始尝试 + 重试次数 = 总尝试次数（`for (attempt = 0; attempt <= max_retries; attempt++)`）。

**Q: LOG_ONLY 模式的行为？**
达到阈值时记录日志，重置计数器，继续监控。

**Q: 为什么编译需要 GCC 14+？**
GCC 14 是首个对 C23 标准提供较完整支持的稳定版本（包括 `-std=c23` 标志、`stdckdint.h`、改进的 `restrict` 分析等）。检查当前版本：`gcc --version && gcc -std=c23 -E -dM - < /dev/null | grep __STDC_VERSION__`

**Q: systemd watchdog 如何工作？**
从 `WATCHDOG_USEC` 环境变量读取超时时间，在可中断休眠循环中周期性发送心跳。

**Q: 为什么需要 EINTR 重试？**
系统调用可能被信号中断（如 `SIGWINCH`），需要在循环中重试 `sendto()` 等调用。

## ⚠️ 已知限制

- 仅支持 ICMP（不支持 TCP/UDP 探测）；某些网络可能过滤 ICMP。
- 目标地址：仅支持 IPv4/IPv6 数字 IP 字面量（不做 DNS 解析）。
- Linux 专属；ICMP raw socket 需要 root 或 `CAP_NET_RAW`。
- 单进程单目标；多目标需要启动多个实例。

## 📄 许可证

本项目采用 MIT 许可证 - 详见 [LICENSE](LICENSE) 文件。
