# OpenUPS - Network Monitor with Auto-Shutdown

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C23](https://img.shields.io/badge/C-23-blue.svg)](https://en.wikipedia.org/wiki/C23_(C_standard_revision))
[![systemd](https://img.shields.io/badge/systemd-integrated-green.svg)](https://systemd.io/)
[![Security](https://img.shields.io/badge/security-10%2F10-brightgreen.svg)](#-安全特性)

**OpenUPS** 是一个**轻量级、高性能、高安全**的 Linux 网络监控工具，通过周期性 ICMP ping 检测网络可达性，并在连续失败达到阈值后自动执行关机或自定义脚本。

## ✨ 特性

### 核心功能
- **原生 ICMP 实现**：使用 raw socket 实现 ICMP ping，无需依赖系统 `ping` 命令
- **IPv4/IPv6 双栈支持**：同时支持 IPv4 和 IPv6 网络
- **智能重试机制**：可配置的重试次数
- **灵活的关机策略**：immediate、delayed、log-only 三种模式

### 性能优势
- **C23 标准**：使用最新的 C 语言标准和编译器优化
- **极致性能**：-O3 + LTO + CPU 原生优化
- **统一上下文架构**：单参数传递，减少函数调用开销
- **CPU 缓存友好**：热路径数据优化布局
- **超小体积**：约 48 KB 二进制文件
- **低资源占用**：< 5 MB 内存，< 1% CPU
- **ICMP 热路径优化**：payload 预填充与缓冲区复用，降低每次 ping 的写入量

### 系统集成
- **systemd 深度集成**：支持 `sd_notify`、watchdog、状态通知
- **安全加固**：10/10 安全评级，Full RELRO + PIE + Stack Canary + FORTIFY_SOURCE=3

### 可靠性
- **信号处理**：优雅处理 SIGINT/SIGTERM/SIGUSR1
- **指标统计**：实时监控成功率、延迟、运行时长
- **dry-run 模式**：默认启用，防止误操作

## 🏗️ 架构（重构版 - 2026-01-25）

```
src/
├── main.c         # 程序入口（简化为 22 行）
├── context.c/h    # 统一上下文管理（核心模块）
├── common.c/h     # 通用工具函数
├── config.c/h     # 配置管理
├── logger.c/h     # 日志系统
├── icmp.c/h       # ICMP ping 实现
├── metrics.c/h    # 指标统计（成功率/延迟/运行时长）
├── shutdown.c/h   # 关机触发（fork/execvp，无 shell）
└── systemd.c/h    # systemd 集成
```

**设计原则**：
- ✅ 统一上下文架构（`openups_ctx_t`）
- ✅ 单参数传递，优化函数调用
- ✅ 内存局部性优化（CPU 缓存友好）
- ✅ 零第三方依赖（仅 C 标准库和 Linux 系统调用）
- ✅ 单一二进制文件，易于部署

**重构改进**：
- 代码量减少 13.3%（487 → 422 行）
- 主程序简化 69%（71 → 22 行）
- 内存占用降低 4.2%
- 启动时间优化 16.7%

详见 [REFACTORING.md](REFACTORING.md)

## 🚀 快速开始

面向首次使用者的最小流程：编译 → 查看帮助 → dry-run 验证 →（可选）安装为 systemd 服务。

### 前置要求

```bash
# Ubuntu/Debian
sudo apt install build-essential

# CentOS/RHEL
sudo yum groupinstall "Development Tools"
```

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
# 1. 编译
make

# 2. 安装二进制文件
sudo cp bin/openups /usr/local/bin/
sudo chmod 755 /usr/local/bin/openups
sudo setcap cap_net_raw+ep /usr/local/bin/openups

# 3. 安装 systemd 服务
sudo cp systemd/openups.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable openups

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
Environment="OPENUPS_DRY_RUN=false"
Environment="OPENUPS_TIMESTAMP=false"

; 权限说明（默认 unit）：
; - service 以 root 运行，但 CapabilityBoundingSet 仅保留 CAP_NET_RAW
; - 关机通过 systemctl/shutdown 完成，不需要 CAP_SYS_BOOT
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

完整参数以 `./bin/openups --help` 为准；更完整的部署/排错/限制说明见 [TECHNICAL.md](TECHNICAL.md#%E8%BF%90%E8%A1%8C%E5%8F%82%E8%80%83)。

| 目标 | 推荐参数 |
|------|----------|
| 调整检测频率 | `--interval <sec>` / `--timeout <ms>` |
| 调整容错阈值 | `--threshold <num>` / `--retries <num>` |
| 生产启用关机 | `--dry-run=false` |
| 选择关机策略 | `--shutdown-mode immediate|delayed|log-only` |
| systemd 集成 | `--systemd[=true/false]` / `--watchdog[=true/false]` |

## 🔒 安全特性

### 方式 1：使用 capability（推荐）

```bash
# 授予 CAP_NET_RAW 权限
sudo setcap cap_net_raw+ep ./bin/openups

# 普通用户即可运行
./bin/openups --target 1.1.1.1
```

### 方式 2：使用 root 权限

```bash
sudo ./bin/openups --target 1.1.1.1
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

## 📚 文档导航

根据您的需求选择合适的文档：

| 文档 | 适用场景 |
|------|----------|
| 📖 [README.md](README.md) | **项目概览** - 首次了解项目特性和快速开始（当前页） |
| 🔧 [TECHNICAL.md](TECHNICAL.md) | **深入开发** - 架构设计、模块详解、开发规范 |
| 🤝 [CONTRIBUTING.md](CONTRIBUTING.md) | **参与贡献** - 如何提交 PR 和 Issue |

## 🧪 测试

运行自动化测试套件：

```bash
./test.sh
```

## 📄 许可证

本项目采用 MIT 许可证 - 详见 [LICENSE](LICENSE) 文件。

## 🤝 贡献

欢迎提交 Issue 和 Pull Request！详见 [CONTRIBUTING.md](CONTRIBUTING.md)。
