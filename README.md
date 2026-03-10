# OpenUPS - 高性能网络监控工具

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C23](https://img.shields.io/badge/C-23-blue.svg)](https://en.wikipedia.org/wiki/C23_(C_standard_revision))
[![systemd](https://img.shields.io/badge/systemd-integrated-green.svg)](https://systemd.io/)

**OpenUPS** 是一个轻量级、高性能的 Linux 网络监控工具，通过周期性 ICMP ping 检测网络可达性，并在连续失败达到阈值后自动执行关机。

## 核心特性

- **原生 ICMP 实现**：使用 raw socket，无需依赖系统 `ping` 命令
- **灵活的关机策略**：支持 dry-run、true-off、log-only 三种模式
- **systemd 深度集成**：支持 `sd_notify`、watchdog、状态通知（watchdog 随 systemd 自动启用）
- **高性能**：单一二进制文件，内存占用 < 5 MB，CPU 占用 < 1%
- **安全加固**：Full RELRO、PIE、Stack Canary、NX、FORTIFY_SOURCE

## 快速开始

### 1. 构建

```bash
make
make release
```

当前 Makefile 只负责本地构建、测试和代码检查，不再处理系统安装/卸载。

### 2. 运行

```bash
# 查看帮助
./bin/openups --help

# 前台调试运行（干跑模式，不实际关机）
sudo ./bin/openups --target 1.1.1.1 --interval 1 --threshold 3 --shutdown-mode dry-run --log-level debug
```

### 3. 可选：手动注册 systemd 服务

如果你需要常驻运行，再手动复制二进制和 service 文件即可：

```bash
sudo cp bin/openups /usr/local/bin/openups
sudo chmod 755 /usr/local/bin/openups
sudo setcap cap_net_raw+ep /usr/local/bin/openups

sudo cp systemd/openups.service /etc/systemd/system/openups.service
sudo systemctl daemon-reload
sudo systemctl enable --now openups
```

随后可编辑 `/etc/systemd/system/openups.service` 里的 `Environment=` 行，再执行：

```bash
sudo systemctl daemon-reload
sudo systemctl restart openups
```

当前服务单元不会再通过固定 `sleep` 人为延迟启动；启动超时默认是 30 秒，失败后按 systemd 的重启策略恢复。

### 4. Make 目标

```bash
make          # 构建 bin/openups
make release  # 构建后 strip
make test     # 运行 ./test.sh
make format   # clang-format
make lint     # cppcheck + clang-tidy
make clean    # 清理 bin/
```

### 5. 测试

```bash
# 基础测试
./test.sh

# 进程级灰度测试（需要 root 或 CAP_NET_RAW）
./test.sh --gray

# systemd 级灰度测试（需要先手动注册服务）
sudo cp systemd/openups.service /etc/systemd/system/openups.service
sudo systemctl daemon-reload
./test.sh --gray-systemd

# 测试完成后如需清理，手动停用并删除服务文件
sudo systemctl disable --now openups
sudo rm -f /etc/systemd/system/openups.service
sudo systemctl daemon-reload

# 清理灰度测试日志
rm -rf graylogs
```

灰度测试会把日志输出到仓库根目录下的 `graylogs/`。systemd 级灰度测试会临时创建 drop-in 覆盖服务环境变量，并在测试结束后自动回滚。

基础测试还会校验 systemd 单元不包含固定启动睡眠，并验证启动超时配置与当前服务定义一致。

## 参数一览

| 参数 | CLI 选项 | 环境变量 | 默认值 | 说明 |
|------|----------|----------|--------|------|
| 监控目标 | `-t, --target` | `OPENUPS_TARGET` | `1.1.1.1` | 目标 IP 字面量（不支持域名） |
| 检测间隔 | `-i, --interval` | `OPENUPS_INTERVAL` | `10`（秒） | 两次 ping 之间的间隔 |
| 失败阈值 | `-n, --threshold` | `OPENUPS_THRESHOLD` | `5` | 连续失败多少次触发关机 |
| 超时时间 | `-w, --timeout` | `OPENUPS_TIMEOUT` | `2000`（ms） | 单次 ping 等待回包的超时 |
| 关机模式 | `-S, --shutdown-mode` | `OPENUPS_SHUTDOWN_MODE` | `dry-run` | `dry-run` / `true-off` / `log-only` |
| 倒计时分钟 | `-D, --delay` | `OPENUPS_DELAY_MINUTES` | `0` | `dry-run` / `true-off` 模式下的程序内倒计时，`0` 表示立即执行 |
| 日志级别 | `-L, --log-level` | `OPENUPS_LOG_LEVEL` | `info` | `silent` / `error` / `warn` / `info` / `debug` |
| systemd 集成 | `-M, --systemd` | `OPENUPS_SYSTEMD` | `true` | 启用 sd_notify 和 watchdog |

## 模式说明

三种模式现在是互斥的，倒计时由 `--delay` 单独控制：

- **`--shutdown-mode dry-run`**：
  达到阈值后只模拟关机动作，不执行真正关机。若 `--delay` 大于 `0`，会先进入程序内部倒计时；倒计时期间网络恢复会取消本次计划动作。
- **`--shutdown-mode true-off`**：
  达到阈值后执行真正关机。若 `--delay` 大于 `0`，同样先由程序内部倒计时，再在到点时执行立即关机。
- **`--shutdown-mode log-only`**：
  当网络失败达到阈值时，它只记录失败警告并**将失败计数器清零，进程继续无限期监控下去**。这种模式使 OpenUPS 退化为一个纯粹的后台网络探针和服务状态采集器，适合配合 systemd 持续监控网络。

`--delay` 现在只控制 `dry-run` 和 `true-off` 的程序内倒计时，不再作为独立关机模式存在。

## 日志时间戳

`OPENUPS_TIMESTAMP` 已并入 `OPENUPS_SYSTEMD` 的行为，不再单独暴露。

- 当 `--systemd=true` 时，日志会进入 journald，OpenUPS 自动关闭前缀时间戳，避免和 systemd/journal 的时间字段重复。
- 当 `--systemd=false` 时，OpenUPS 自动打开时间戳，便于前台运行、重定向文件和手工排障。

也就是说，时间戳现在是派生行为，而不是单独的 CLI 或环境变量配置项。

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

