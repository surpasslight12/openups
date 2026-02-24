# OpenUPS - 高性能网络监控工具

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C23](https://img.shields.io/badge/C-23-blue.svg)](https://en.wikipedia.org/wiki/C23_(C_standard_revision))
[![systemd](https://img.shields.io/badge/systemd-integrated-green.svg)](https://systemd.io/)

**OpenUPS** 是一个轻量级、高性能的 Linux 网络监控工具，通过周期性 ICMP ping 检测网络可达性，并在连续失败达到阈值后自动执行关机或自定义脚本。

## ✨ 核心特性

- **原生 ICMP 实现**：使用 raw socket，无需依赖系统 `ping` 命令。
- **灵活的关机策略**：支持 immediate、delayed、log-only 三种模式。
- **systemd 深度集成**：支持 `sd_notify`、watchdog、状态通知。
- **高性能**：单一二进制文件，内存占用 < 5 MB，CPU 占用 < 1%。
- **安全加固**：10/10 安全评级，支持 Full RELRO、PIE、Stack Canary 等。

## 🚀 快速开始

### 编译与运行

```bash
# 安装编译工具
sudo apt install build-essential

# 编译
make

# 查看帮助
./bin/openups --help

# 测试运行
sudo ./bin/openups --target 1.1.1.1 --interval 1 --threshold 3 --dry-run --log-level debug
```

### 安装为系统服务

```bash
sudo make install
sudo systemctl enable --now openups
sudo systemctl status openups
```

## ⚙️ 默认参数

| 参数         | CLI 选项              | 环境变量              | 默认值       | 说明                     |
|--------------|-----------------------|-----------------------|--------------|--------------------------|
| 监控目标     | `-t, --target`        | `OPENUPS_TARGET`      | `1.1.1.1`    | 目标 IP 字面量（不支持域名） |
| 检测间隔     | `-i, --interval`      | `OPENUPS_INTERVAL`    | `10`（秒）   | 两次 ping 之间的间隔     |
| 失败阈值     | `-n, --threshold`     | `OPENUPS_THRESHOLD`   | `5`          | 连续失败多少次触发关机   |
| 超时时间     | `-w, --timeout`       | `OPENUPS_TIMEOUT`     | `2000`（ms） | 单次 ping 等待回包的超时 |
| 关机模式     | `-S, --shutdown-mode` | `OPENUPS_SHUTDOWN_MODE` | `immediate` | `immediate` / `delayed` / `log-only` |
| 演习模式     | `-d, --dry-run`       | `OPENUPS_DRY_RUN`     | `true`       | 不执行实际关机（安全默认值） |

## 📄 许可证

本项目采用 MIT 许可证 - 详见 [LICENSE](LICENSE) 文件。
