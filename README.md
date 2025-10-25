# OpenUPS C - Network Monitor with Auto-Shutdown

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C11](https://img.shields.io/badge/C-11-blue.svg)](https://en.wikipedia.org/wiki/C11_(C_standard_revision))
[![systemd](https://img.shields.io/badge/systemd-integrated-green.svg)](https://systemd.io/)

**OpenUPS C** 是 OpenUPS 项目的 C 语言实现版本，是一个轻量级、高性能的 Linux 网络监控工具，通过周期性 ICMP ping 检测网络可达性，并在连续失败达到阈值后自动执行关机或自定义脚本。

## ✨ 特性

### 核心功能
- **原生 ICMP 实现**：使用 raw socket 实现 ICMP ping，无需依赖系统 `ping` 命令
- **IPv4/IPv6 双栈支持**：同时支持 IPv4 和 IPv6 网络
- **智能重试机制**：可配置的重试次数
- **灵活的关机策略**：immediate、delayed、log-only、custom 四种模式

### 系统集成
- **systemd 深度集成**：支持 `sd_notify`、watchdog、状态通知
- **Syslog 日志**：可选的 syslog 输出，便于集中日志管理
- **安全加固**：最小权限运行（仅需 `CAP_NET_RAW`）

### 可靠性
- **信号处理**：优雅处理 SIGINT/SIGTERM/SIGUSR1
- **指标统计**：实时监控成功率、延迟、运行时长
- **Dry-run 模式**：默认启用，防止误操作

## 🏗️ 架构

```
src/
├── main.c         # 程序入口
├── common.c/h     # 通用工具函数
├── config.c/h     # 配置管理
├── logger.c/h     # 日志系统
├── icmp.c/h       # ICMP ping 实现
├── systemd.c/h    # systemd 集成
└── monitor.c/h    # 监控核心逻辑
```

**设计原则**：
- 模块化架构，职责清晰
- 零第三方依赖（仅 C11 标准库和 Linux 系统调用）
- 单一二进制文件，易于部署

## 🚀 快速开始

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

### 测试运行

```bash
# 测试 ICMP ping（需要 root 或 CAP_NET_RAW）
sudo ./bin/openups --target 1.1.1.1 --interval 1 --threshold 3 --dry-run --verbose

# 或授予 capability
sudo setcap cap_net_raw+ep ./bin/openups
./bin/openups --target 1.1.1.1 --interval 1 --threshold 3 --dry-run
```

### 安装为系统服务

```bash
# 1. 编译并安装二进制文件
make
sudo make install

# 2. 安装 systemd 服务（如果提供）
# 参考 systemd/ 目录下的配置文件

# 查看帮助
openups --help
```

## 📖 使用示例

### 基本监控

```bash
# 监控 Cloudflare DNS，10 秒间隔，5 次失败触发
./bin/openups --target 1.1.1.1 --interval 10 --threshold 5
```

### 实际关机模式

```bash
# 禁用 dry-run，真实执行关机
sudo ./bin/openups --target 192.168.1.1 --interval 5 --threshold 3 --no-dry-run
```

### IPv6 监控

```bash
./bin/openups --target 2606:4700:4700::1111 --ipv6 --interval 10
```

### 自定义关机脚本

```bash
./bin/openups --shutdown-mode custom --custom-script /usr/local/bin/my-shutdown.sh --no-dry-run
```

## ⚙️ 配置参数

**配置优先级**：CLI 参数 > 环境变量 > 默认值

| 参数 | 环境变量 | 默认值 | 说明 |
|------|---------|--------|------|
| `--target` | `OPENUPS_TARGET` | `1.1.1.1` | 监控目标主机/IP |
| `--interval` | `OPENUPS_INTERVAL` | `10` | ping 间隔（秒） |
| `--threshold` | `OPENUPS_THRESHOLD` | `5` | 连续失败阈值 |
| `--timeout` | `OPENUPS_TIMEOUT` | `2000` | 单次超时（毫秒） |
| `--packet-size` | `OPENUPS_PACKET_SIZE` | `56` | ICMP payload 大小 |
| `--retries` | `OPENUPS_RETRIES` | `2` | 每轮重试次数 |
| `--shutdown-mode` | `OPENUPS_SHUTDOWN_MODE` | `immediate` | 关机模式 |
| `--dry-run` | `OPENUPS_DRY_RUN` | `true` | Dry-run 模式 |
| `--ipv6` | `OPENUPS_IPV6` | `false` | 使用 IPv6 |
| `--syslog` | `OPENUPS_SYSLOG` | `false` | 启用 syslog |
| `--log-level` | `OPENUPS_LOG_LEVEL` | `info` | 日志级别 |

完整参数列表：`./bin/openups --help`

## 🔒 权限与安全

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
./bin/openups --log-level debug --verbose --target 127.0.0.1
```

## 🛠️ 开发

### 构建调试版本

```bash
make clean
make CC=gcc CFLAGS="-g -O0 -std=c11 -Wall -Wextra"
```

### 代码结构

- **icmp.c/h**：ICMP ping 核心实现，支持 IPv4/IPv6
- **systemd.c/h**：systemd 通知和 watchdog 集成
- **monitor.c/h**：监控循环和关机触发逻辑
- **config.c/h**：配置解析和验证
- **logger.c/h**：结构化日志（支持 syslog）
- **common.c/h**：通用工具函数

## 📝 更新日志

**当前版本**：v1.0.0

### 主要特性
- 🎯 完整的 C 语言实现
- 🌐 原生 ICMP 实现（IPv4/IPv6）
- ⚙️ 深度 systemd 集成（notify、watchdog）
- 🔒 安全加固（CAP_NET_RAW）
- 📝 结构化日志系统

## 📄 许可证

本项目采用 MIT 许可证 - 详见 [LICENSE](LICENSE) 文件。

## 🤝 贡献

欢迎提交 Issue 和 Pull Request！

## 📚 相关项目

- [openups_cpp](https://github.com/surpasslight12/openups_cpp) - C++ 实现版本（原始版本）
- [openups_rust](https://github.com/surpasslight12/openups_rust) - Rust 实现版本
- [openups_python](https://github.com/surpasslight12/openups_python) - Python 实现版本

---

**作者**: OpenUPS Project  
**维护**: surpasslight12
