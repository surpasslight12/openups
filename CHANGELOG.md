# Changelog

所有重要的项目变更都将记录在此文件中。

格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.0.0/)，
版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

---

## [Unreleased]

### Added
- **代码注释全面增强** 📝
  - 为 monitor.c 添加详细的监控循环、重试机制、信号处理、watchdog 心跳注释
  - 为 icmp.c 添加 ICMP 校验和算法、IPv4/IPv6 协议实现的深入注释
  - 为 config.c 添加参数优先级、验证逻辑、安全性检查的详细说明
  - 为 systemd.c 添加 UNIX domain socket、通知机制、watchdog 超时的注释
  - 为 main.c 添加程序初始化流程、配置加载顺序的注释
  - 总计 176 个注释块，覆盖所有关键函数和复杂算法
  - 提升代码可读性，便于新开发者快速上手
  - 代码行数从 1,947 增加到 2,027（+80 行，主要是新增注释）

- **关机命令优化** ⚡
  - 当启用 systemd 集成时，自动优先使用 `systemctl poweroff` 替代 `/sbin/shutdown`
  - 提供更优雅和一致的关机方式
  - 在传统 Linux 环境中继续使用 `/sbin/shutdown` 命令

### Removed
- **移除 syslog 相关代码** 🧹
  - 完全移除 `use_syslog` 配置项和相关代码
  - 移除 `--syslog` CLI 参数和 `-Y` 短选项
  - 移除 `OPENUPS_SYSLOG` 环境变量
  - 移除 systemd 服务中的 `SyslogIdentifier` 和 `SyslogLevelPrefix` 配置
  - 原因：stderr 已通过 systemd 的 `StandardOutput=journal` 和 `StandardError=journal` 自动捕获到 journalctl，实现完全冗余

### Changed
- **日志系统简化**
  - logger.c 不再包含 `<syslog.h>`
  - 日志仅输出到 stderr，由 systemd 自动转向 journalctl
  - 代码行数减少 ~30 行，维护负担降低

### Documentation
- 📝 移除所有 SYSLOG_*.md 和 SYSLOG_*.txt 文档文件
- 📝 README.md: 移除 syslog 相关章节和配置说明
- 📝 QUICKSTART.md: 移除"场景 4.5: Syslog 集中日志管理"
- 📝 TECHNICAL.md: 更新 logger 模块说明，移除 syslog 特性描述
- 📝 .github/copilot-instructions.md: 同步移除 syslog 相关指导
- 📝 test.sh: 更新测试套件，移除 syslog 测试，添加新参数测试

---

## [1.2.0] - 2025-11-04

### 🎯 重大更新：CLI 参数系统全面重构与简化

本版本对 CLI 参数系统进行了全面重构，并最终简化为仅使用 `true/false` 布尔值，大大降低学习难度。

### Changed
- **CLI 参数系统简化** ⚡
  - 所有布尔参数统一为仅支持 `true|false`（不区分大小写）
  - 移除了 yes/no, 1/0, on/off 等多种冗余格式
  - 简化布尔参数解析逻辑，代码更清晰
  - 用户学习成本降低，使用体验更一致

- **布尔参数列表** 📋
  - `-6, --ipv6[=true|false]` - IPv6 模式
  - `-d, --dry-run[=true|false]` - Dry-run 模式
  - `-T, --timestamp[=true|false]` - 日志时间戳
  - `-M, --systemd[=true|false]` - systemd 集成
  - `-W, --watchdog[=true|false]` - systemd watchdog

- **环境变量系统完善** 📝
  - 所有布尔环境变量现在仅接受 `true|false` 值
  - 更新：`OPENUPS_DRY_RUN`, `OPENUPS_TIMESTAMP`, `OPENUPS_SYSTEMD`, `OPENUPS_WATCHDOG`, `OPENUPS_IPV6`
  - 保持配置优先级：CLI 参数 > 环境变量 > 默认值

- **文档全面更新** 📚
  - README.md: 所有参数表格改为 `true|false` 格式
  - QUICKSTART.md: 所有示例改为新参数格式
  - systemd/openups.service: 更新环境变量为 true/false
  - .github/copilot-instructions.md: 同步参数说明

### Documentation
- 📝 README.md: 更新参数表格和所有示例
- 📝 QUICKSTART.md: 更新 5 个场景示例
- 📝 systemd/openups.service: 更新环境变量配置
- 📝 .github/copilot-instructions.md: 更新参数说明

### Performance
- 📊 二进制大小维持：**39 KB**
- 📊 内存占用维持：**< 5 MB**
- 📊 安全评分维持：**10/10** 🏆
- 📊 编译警告：**0** ✅

---

## [1.1.1] - 2025-10-27

### Changed
- **CLI 参数系统优化**: 完善短选项支持
  - 所有长选项现在都支持对应的短选项
  - 短选项列表：`-t -i -n -w -s -r -S -D -C -P -L -6 -d -N -Y -T -M -W -Z -h`

### Fixed
- **GCC 13 兼容性**: 为 stdckdint.h (C23) 添加后备实现

---

## [1.1.0] - 2025-10-26

### ⚡ 性能升级 & 代码优化

#### Changed
- **日志系统全面重构**: 从 key=value 格式改为自然语序
- **日志系统简化**: 统一使用 `--log-level` 参数（silent|error|warn|info|debug）
- **C 标准升级**: C11 → **C2x (C23)**
- **优化级别提升**: -O2 → **-O3** + LTO + 原生优化

#### Security
- 🔐 **安全加固升级**: Full RELRO, PIE, Stack Canary, NX Stack, FORTIFY_SOURCE=3

#### Performance
- 📉 **二进制大小优化**: 45KB → **39KB (-13%)**

---

## [1.0.0] - 2025-10-26

### 🎉 首次发布

#### Added
- ✨ **完整的 C 语言实现**（7 个模块化组件）
- 🌐 **原生 ICMP 实现**（IPv4/IPv6 支持）
- ⚙️ **systemd 深度集成**（notify、watchdog）
- 📝 **自然语序日志系统**（printf 风格可变参数）
- 🔧 **灵活的配置系统**（CLI + 环境变量）
- 🛡️ **四种关机模式**（immediate、delayed、log-only、custom）
- 📊 **实时监控指标**（成功率、延迟、运行时长）
- 🔒 **安全特性**（CAP_NET_RAW、Dry-run、输入验证）

#### Technical
- **语言**：C11 标准 | **代码量**：~1685 行（v1.0.0） | **二进制**：45 KB
- **安全**：10/10 | **依赖**：零第三方

---
