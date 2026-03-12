# OpenUPS - 高性能网络监控工具

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C23](https://img.shields.io/badge/C-23-blue.svg)](https://en.wikipedia.org/wiki/C23_(C_standard_revision))
[![systemd](https://img.shields.io/badge/systemd-integrated-green.svg)](https://systemd.io/)

**OpenUPS** 是一个轻量级、高性能的 Linux 网络监控工具，通过周期性 ICMP ping 检测网络可达性，并在连续失败达到阈值后自动执行关机。

## 核心特性

- **原生 ICMP 实现**：使用 raw socket + BPF 内核过滤，无需依赖系统 `ping` 命令
- **灵活的关机策略**：支持 `dry-run`、`true-off`、`log-only` 三种模式，`--delay` 独立控制程序内倒计时
- **systemd 深度集成**：支持 `sd_notify`、watchdog、状态通知；watchdog 随 systemd 自动启用
- **高性能**：单一二进制文件 ≈ 48 KB，内存占用 < 5 MB，CPU 占用 < 1%
- **安全加固**：编译期 Full RELRO、PIE、Stack Canary、NX、FORTIFY\_SOURCE；运行期 systemd 沙箱全量启用

## 快速开始

### 1. 构建

```bash
make          # 构建 bin/openups
make release  # 构建后 strip
```

### 2. 运行

```bash
# 查看帮助
./bin/openups --help

# 前台调试运行（干跑模式，不实际关机）
sudo ./bin/openups --target 1.1.1.1 --interval 10 --timeout 2000 \
     --threshold 3 --shutdown-mode dry-run --log-level debug
```

### 3. 可选：手动注册 systemd 服务

```bash
sudo cp bin/openups /usr/local/bin/openups
sudo cp systemd/openups.service /etc/systemd/system/openups.service
sudo systemctl daemon-reload
sudo systemctl enable --now openups
```

服务启动后可通过 drop-in 覆盖 `Environment=` 行来修改配置，无需改动原始 unit 文件：

```bash
sudo systemctl edit openups
# 在弹出的编辑器里写入：
# [Service]
# Environment="OPENUPS_TARGET=192.168.1.1"
# Environment="OPENUPS_SHUTDOWN_MODE=true-off"

sudo systemctl daemon-reload
sudo systemctl restart openups
```

查看实时日志：

```bash
journalctl -fu openups
```

### 4. Make 目标

| 目标 | 说明 |
|------|------|
| `make` | 构建 `bin/openups` |
| `make release` | 构建后 strip |
| `make test` | 运行 `./test.sh` |
| `make format` | clang-format |
| `make lint` | cppcheck + clang-tidy |
| `make clean` | 清理 `bin/` |

### 5. 测试

```bash
# 基础测试（27 项，无需 root）
./test.sh

# 进程级灰度测试（需要 root 或 CAP_NET_RAW）
sudo ./test.sh --gray

# systemd 级灰度测试（需要先手动注册服务，且 systemd 环境下 root 执行）
sudo cp systemd/openups.service /etc/systemd/system/openups.service
sudo systemctl daemon-reload
sudo ./test.sh --gray-systemd

# 全量测试（基础 + 灰度 + systemd 灰度）
sudo ./test.sh --all

# 清理灰度测试日志
rm -rf graylogs
```

灰度测试日志输出至仓库根 `graylogs/`。systemd 级灰度测试会临时创建 drop-in 覆盖服务环境变量，并在结束后自动回滚。

## 参数一览

| 参数 | CLI 选项 | 环境变量 | 默认值 | 说明 |
|------|----------|----------|--------|------|
| 监控目标 | `-t, --target` | `OPENUPS_TARGET` | `1.1.1.1` | 目标 IP 字面量（仅支持 IPv4/IPv6，不解析域名） |
| 检测间隔 | `-i, --interval` | `OPENUPS_INTERVAL` | `10`（秒） | 两次 ping 之间的间隔 |
| 失败阈值 | `-n, --threshold` | `OPENUPS_THRESHOLD` | `5` | 连续失败次数触发关机 |
| 超时时间 | `-w, --timeout` | `OPENUPS_TIMEOUT` | `2000`（ms） | 单次 ping 等待回包的超时，必须小于 interval |
| 关机模式 | `-S, --shutdown-mode` | `OPENUPS_SHUTDOWN_MODE` | `dry-run` | `dry-run` / `true-off` / `log-only` |
| 倒计时分钟 | `-D, --delay` | `OPENUPS_DELAY_MINUTES` | `0` | 程序内关机倒计时（分钟），`0` 表示立即执行；对 `log-only` 无效 |
| 日志级别 | `-L, --log-level` | `OPENUPS_LOG_LEVEL` | `info` | `silent` / `error` / `warn` / `info` / `debug` |
| systemd 集成 | `-M, --systemd` | `OPENUPS_SYSTEMD` | `true` | 启用 `sd_notify`、watchdog 与状态通知 |

优先级规则：CLI 参数 > 环境变量 > 编译期默认值。

## 关机模式说明

### `dry-run`
达到阈值后模拟关机流程，但不实际执行关机命令。  
`--delay > 0` 时先启动程序内倒计时；在倒计时结束前网络恢复可取消本次计划动作。

### `true-off`
达到阈值后执行真正关机。  
systemd 环境下调用 `systemctl poweroff`，否则回退至 `/sbin/shutdown`。  
`--delay > 0` 时同样先进行程序内倒计时，届时立即执行关机。

### `log-only`
阈值触发时只记录警告日志并**重置失败计数器**，进程续续监控，永不执行关机。  
适用于将 OpenUPS 作为纯网络探针或配合外部告警系统使用的场景。

## 日志时间戳行为

`OPENUPS_TIMESTAMP` 已移除，时间戳现为**派生行为**：

- `--systemd=true`：日志进入 journald，OpenUPS 自动**关闭**前缀时间戳（避免与 journal 时间字段重复）
- `--systemd=false`：OpenUPS 自动**开启**时间戳，便于前台运行、重定向文件和手工排障

## 信号处理

| 信号 | 行为 |
|------|------|
| `SIGTERM` | 优雅停止，输出最终统计后退出 |
| `SIGINT` | 同 `SIGTERM` |
| `SIGUSR1` | 立即输出当前统计信息（成功率、平均延迟、运行时间），不中断监控 |

## systemd 服务单元

`systemd/openups.service` 启用了完整的沙箱隔离：

### 进程能力

| 能力 | 用途 |
|------|------|
| `CAP_NET_RAW` | ICMP raw socket |
| `CAP_SYS_BOOT` | `/sbin/shutdown` 回退路径（调用 `reboot()` 系统调用） |

### 文件系统 / 命名空间隔离

`PrivateTmp`、`PrivateDevices`、`PrivateMounts`、`ProtectSystem=strict`、`ProtectHome`、`ProtectHostname`、`ProtectClock`、`ProtectKernelModules`、`ProtectKernelTunables`、`ProtectKernelLogs`、`ProtectControlGroups`、`ProtectProc=invisible`、`ProcSubset=pid`

### 地址空间 / IPC 限制

`LockPersonality`、`MemoryDenyWriteExecute`、`RestrictNamespaces`、`RestrictRealtime`、`RestrictSUIDSGID`、`RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6 AF_NETLINK`、`KeyringMode=private`、`RemoveIPC`

### 系统调用过滤

白名单：`@system-service @network-io @process @reboot`  
黑名单：`@debug @module @mount @swap @obsolete @cpu-emulation`

### 资源限制

`MemoryMax=50M`、`TasksMax=10`、`OOMScoreAdjust=-100`（防止被 OOM killer 杀死）

## 项目结构

```
src/
├── openups.h        # 公共类型与 API 声明
├── config.c         # 参数解析、校验、渲染
├── monitor.c        # 监控主循环（metrics、状态机、shutdown FSM、reactor）
├── icmp.c           # ICMP raw socket、BPF 过滤、校验和
├── logger.c         # 日志、单调时钟、时间戳
├── shutdown.c       # 关机执行（posix_spawn）
├── systemd.c        # systemd notify socket 集成
├── monitor.h        # monitor 模块公开 API
└── main.c           # 入口
systemd/
└── openups.service  # systemd unit 文件
```

## 许可证

本项目采用 MIT 许可证，详见 [LICENSE](LICENSE)。

## 更新日志

### v1.3.0 架构合并与安全审计

- **文件结构精简**：21 个源文件合并为 9 个，消除过度拆分；`monitor.c` 内聚所有监控逻辑（metrics、状态机、shutdown FSM、reactor）
- **代码审计修复**：
  - `log_message` 新增日志级别过滤（直接调用 `logger_log_va` 时曾绕过级别检查）
  - `shutdown_fsm_execute` 移除不可达的 `LOG_ONLY` 死代码分支
  - `shutdown_fsm_handle_threshold` 的 log-only 路径补全警告日志，使触发行为可观测
- **测试覆盖**：27 项基础测试 + 6 阶段进程级灰度测试全部通过
- **systemd 单元强化**：新增 `ProtectProc=invisible`、`ProcSubset=pid`、`ProtectHostname`、`ProtectClock`、`PrivateMounts`、`KeyringMode=private`、`RemoveIPC`、`OOMScoreAdjust=-100`、`SystemCallFilter` 白/黑名单；移除多余的 `CAP_KILL`；补全 `SyslogIdentifier`、`UMask`

### v1.2.0 配置精简与风格统一

- **关机策略优化**：systemd 环境下自动使用 `systemctl poweroff`，非 systemd 回退到 `/sbin/shutdown`
- **watchdog 合并**：移除独立的 `--watchdog` / `OPENUPS_WATCHDOG`，watchdog 随 systemd 集成自动启用
- **时间戳合并**：移除独立的 `OPENUPS_TIMESTAMP`，改为从 `OPENUPS_SYSTEMD` 派生
- **代码风格统一**：全项目统一为 always-braces 风格

### v1.1.0 架构升级与内核优化

- **内核级优化**：引入 BPF `SO_ATTACH_FILTER` 过滤无关 ICMP 流量，高噪声环境下零 CPU 占用
- **系统调用削减**：主循环时钟切换至 `CLOCK_MONOTONIC_COARSE`，利用 vDSO 免陷入优化
- **灾变抗性**：关机逻辑改用 `posix_spawn` + fd redirect，替代传统 `fork/exec`，应对 OOM 场景
- **配置精简**：移除 `PAYLOAD_SIZE`，自动推导 64 字节固定探测包

