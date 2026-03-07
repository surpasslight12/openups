# OpenUPS - 高性能网络监控工具

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C23](https://img.shields.io/badge/C-23-blue.svg)](https://en.wikipedia.org/wiki/C23_(C_standard_revision))
[![systemd](https://img.shields.io/badge/systemd-integrated-green.svg)](https://systemd.io/)

**OpenUPS** 是一个轻量级、高性能的 Linux 网络监控工具，通过周期性 ICMP ping 检测网络可达性，并在连续失败达到阈值后自动执行关机。

## 核心特性

- **原生 ICMP 实现**：使用 raw socket，无需依赖系统 `ping` 命令
- **灵活的关机策略**：支持 immediate、delayed、log-only 三种模式
- **systemd 深度集成**：支持 `sd_notify`、watchdog、状态通知（watchdog 随 systemd 自动启用）
- **高性能**：单一二进制文件，内存占用 < 5 MB，CPU 占用 < 1%
- **安全加固**：Full RELRO、PIE、Stack Canary、NX、FORTIFY_SOURCE

## 快速开始

### 安装与管理服务

先以普通用户完成构建，再以 root 安装 systemd 服务；这样可以避免源码目录里的构建产物被 root 接管。安装后编辑 `/etc/systemd/system/openups.service` 中的 `Environment=` 行来调整参数，然后 `sudo systemctl daemon-reload && sudo systemctl restart openups`：

```bash
# 先构建
make release

# 再安装 systemd 服务
sudo make install

# 更新构建并重启服务
make release
sudo make update

# 卸载系统服务
sudo make uninstall
```

### 手动调试运行

```bash
# 编译
make

# 查看帮助
./bin/openups --help

# 测试运行（干跑模式不关机）
sudo ./bin/openups --target 1.1.1.1 --interval 1 --threshold 3 --dry-run --log-level debug
```

### 测试与灰度验证

```bash
# 基础测试
./test.sh

# 进程级灰度测试（需要 root 或 CAP_NET_RAW）
./test.sh --gray

# systemd 级灰度测试（需要先安装服务）
make release
sudo make install
./test.sh --gray-systemd

# 测试完成后清理 systemd 安装
sudo make uninstall
```

灰度测试会把日志输出到仓库根目录下的 `graylogs/`。systemd 级灰度测试会临时创建 drop-in 覆盖服务环境变量，并在测试结束后自动回滚；如果只想清理日志文件，执行：

```bash
rm -rf graylogs
```

## 参数一览

| 参数 | CLI 选项 | 环境变量 | 默认值 | 说明 |
|------|----------|----------|--------|------|
| 监控目标 | `-t, --target` | `OPENUPS_TARGET` | `1.1.1.1` | 目标 IP 字面量（不支持域名） |
| 检测间隔 | `-i, --interval` | `OPENUPS_INTERVAL` | `10`（秒） | 两次 ping 之间的间隔 |
| 失败阈值 | `-n, --threshold` | `OPENUPS_THRESHOLD` | `5` | 连续失败多少次触发关机 |
| 超时时间 | `-w, --timeout` | `OPENUPS_TIMEOUT` | `2000`（ms） | 单次 ping 等待回包的超时 |
| 关机模式 | `-S, --shutdown-mode` | `OPENUPS_SHUTDOWN_MODE` | `immediate` | `immediate` / `delayed` / `log-only` |
| 延迟分钟 | `-D, --delay` | `OPENUPS_DELAY_MINUTES` | `1` | delayed 模式下的关机等待分钟 |
| 演习模式 | `-d, --dry-run` | `OPENUPS_DRY_RUN` | `true` | 不执行实际关机（安全默认值） |
| 日志级别 | `-L, --log-level` | `OPENUPS_LOG_LEVEL` | `info` | `silent` / `error` / `warn` / `info` / `debug` |
| 时间戳 | `-T, --timestamp` | `OPENUPS_TIMESTAMP` | `true` | 日志是否包含时间戳 |
| systemd 集成 | `-M, --systemd` | `OPENUPS_SYSTEMD` | `true` | 启用 sd_notify 和 watchdog |

## `log-only` 与 `dry-run` 的区别

尽管两者都不会真正执行关机操作，但它们在设计用途上完全不同：

- **`--dry-run=true`（演习模式）**：
  它是 `immediate` 或 `delayed` 模式的安全测试开关。当网络失败次数达到阈值时，程序会打印"模拟触发了关机"的消息，**并直接退出监控运行**。这用于测试触发链路的可用性（就像电脑真的关机了一样）。
- **`--shutdown-mode log-only`（纯日志监视模式）**：
  当网络失败达到阈值时，它只记录失败警告并**将失败计数器清零，进程继续无限期监控下去**。这种模式使 OpenUPS 退化为一个纯粹的后台网络探针和服务状态采集器，适合配合 systemd 持续监控网络。

## 许可证

本项目采用 MIT 许可证 - 详见 [LICENSE](LICENSE) 文件。

## 更新日志

### v1.2.0 配置精简与风格统一

- **关机策略优化**：systemd 环境下自动使用 `systemctl poweroff`（含 `--when` 延迟支持），非 systemd 回退到 `/sbin/shutdown`
- **watchdog 合并**：移除独立的 `--watchdog` 选项和 `OPENUPS_WATCHDOG` 环境变量，watchdog 随 systemd 集成自动启用
- **代码风格统一**：全项目统一为 always-braces 风格

### v1.1.0 架构升级与内核优化

- **内核级优化**：引入 BPF `SO_ATTACH_FILTER` 过滤无关 ICMP 流量，高噪声环境下零 CPU 占用
- **系统调用削减**：主循环时钟切换至 `CLOCK_MONOTONIC_COARSE`，利用 vDSO 免陷入优化
- **灾变抗性**：关机逻辑改用 `posix_spawn` + fd redirect，替代传统 `fork/exec`，应对 OOM 场景
- **配置精简**：移除 `PAYLOAD_SIZE`，自动推导 64 字节固定探测包
- **架构重写**：单文件拆分为多模块（`icmp.c`、`config.c`、`utils.c` 等）

