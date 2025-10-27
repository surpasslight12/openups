# OpenUPS C 架构说明

**版本**: v1.1.0  
**C 标准**: C23 (C2x)  
**更新**: 2025-10-26

## 概览

OpenUPS C 是 OpenUPS 项目的 C 语言实现版本，采用**模块化架构**设计：

- **模块化设计**：7 个独立模块，职责单一
- **零第三方依赖**：仅使用 C23 标准库和 Linux 系统调用
- **原生 ICMP**：raw socket 实现，无需系统 ping 命令
- **systemd 深度集成**：sd_notify、watchdog、状态通知
- **高性能优化**：-O3 + LTO + CPU native 优化
- **安全加固**：10/10 安全评分，Full RELRO + PIE + Stack Canary

---

## 目录结构

```
openups_c/
├── src/
│   ├── main.c         # 程序入口
│   ├── common.c/h     # 通用工具函数
│   ├── logger.c/h     # 日志系统（支持 syslog）
│   ├── config.c/h     # 配置管理
│   ├── icmp.c/h       # ICMP ping 实现
│   ├── systemd.c/h    # systemd 集成
│   └── monitor.c/h    # 监控核心逻辑
├── systemd/
│   ├── openups.service  # systemd 服务文件
│   ├── install.sh       # 安装脚本
│   └── uninstall.sh     # 卸载脚本
├── docs/
│   └── ARCHITECTURE.md  # 本文件
├── Makefile           # 构建系统
├── README.md          # 项目说明
└── LICENSE            # MIT 许可证
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
    bool use_syslog;
} logger_t;

void logger_init(logger_t* logger, ...);
void logger_info(logger_t* logger, const char* fmt, ...)
    __attribute__((format(printf, 2, 3)));
```

**特性**：
- 同时输出到控制台和 syslog
- printf 风格的可变参数格式化
- 自然语序输出（不使用 key=value 格式）
- 可配置日志级别（SILENT/ERROR/WARN/INFO/DEBUG）
- 编译时格式检查（`__attribute__((format(printf, 2, 3)))`）

**日志格式**：`[TIMESTAMP] [LEVEL] natural language message`

**示例**：
- `[2025-10-27 22:08:23.466] [INFO] Starting OpenUPS monitor: target=127.0.0.1 interval=1s threshold=3 ipv6=false`
- `[2025-10-27 22:08:23.566] [DEBUG] Ping successful to 127.0.0.1, latency: 0.01ms`

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

## 依赖关系图

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

---

## 数据流

### 启动流程
```
1. main() 解析 CLI 参数
2. config_load_from_env() 合并环境变量
3. config_validate() 验证配置
4. logger_init() 初始化日志
5. monitor_init() 初始化监控器（包括 ICMP pinger、systemd notifier）
6. systemd_notifier_ready() 通知 systemd
7. 进入主循环
```

### 监控循环
```
1. icmp_pinger_ping() 执行 ICMP ping
2. handle_ping_success/failure() 处理结果
3. metrics_record_success/failure() 更新指标
4. 检查失败阈值 → trigger_shutdown()
5. 固定间隔休眠
6. 休眠期间每秒发送 watchdog 心跳
```

### 关机流程
```
1. 连续失败达到阈值
2. trigger_shutdown() 被调用
3. 根据 shutdown_mode_t 执行：
   - IMMEDIATE: 立即关机
   - DELAYED: 延迟关机
   - LOG_ONLY: 仅记录日志
   - CUSTOM: 执行自定义脚本
4. 记录日志（warn 级别）
5. 执行系统命令（如非 dry-run）
```

---

## 性能考虑

### 内存使用
- **总体**：< 5 MB（主要是静态分配）
- **ICMP 缓冲区**：~4 KB（接收缓冲区）
- **配置结构**：< 2 KB

### CPU 使用
- **监控循环**：几乎为 0（大部分时间在休眠）
- **ICMP ping**：微秒级操作

### 网络流量
- **典型配置**（10 秒间隔，56 字节 payload）：< 1 KB/分钟

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

### 错误处理
- 所有系统调用检查返回值
- 详细的错误消息
- 避免缓冲区溢出（使用 `strncpy`、`snprintf`）

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

## 与 C++ 版本的差异

| 特性 | C++ 版本 | C 版本 |
|------|---------|--------|
| 语言标准 | C++17 | C11 |
| 类/结构体 | 类（带方法） | 结构体（纯数据） |
| 字符串 | `std::string` | 字符数组 |
| 容器 | `std::vector`, `std::map` | 静态数组 |
| 内存管理 | RAII, 智能指针 | 手动管理 |
| 异常处理 | 异常 | 返回值 + errno |
| 模板 | 泛型编程 | 无（使用宏或 void*） |

---

**版本**：v1.0.0  
**最后更新**：2025-10-26
