# Changelog

所有重要的项目变更都将记录在此文件中。

格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.0.0/)，
版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

---

## [Unreleased]

---

## [1.2.0] - 2025-11-04

### 🎯 重大更新：CLI 参数系统全面重构

本版本对 CLI 参数系统进行了全面重构和优化，提供了更直观、更灵活的配置方式，同时保持向后兼容性（环境变量）。

### Changed
- **CLI 参数系统重构** ⚡
  - 合并 `--dry-run`/`--no-dry-run` → `--dry-run[=yes|no]`
  - 合并 `--no-timestamp` → `--timestamp[=yes|no]`
  - 合并 `--no-systemd` → `--systemd[=yes|no]`
  - 合并 `--no-watchdog` → `--watchdog[=yes|no]`
  - 所有布尔参数支持 4 种格式：yes|no, true|false, 1|0, on|off（大小写不敏感）
  - 短参数统一：`-v` (version), `-M` (systemd), `-W` (watchdog), `-T` (timestamp)
  - 长参数别名简化：`--delay` (delay-minutes), `--script` (custom-script)

- **环境变量系统完善** 📝
  - 新增 `OPENUPS_WATCHDOG` - 控制 systemd watchdog 功能
  - 新增 `OPENUPS_TIMESTAMP` - 统一控制日志时间戳
  - 所有 14 个 CLI 参数现在都有对应的环境变量
  - 配置优先级明确：CLI 参数 > 环境变量 > 默认值

- **帮助文档全面优化** 📚
  - 按功能分为 5 个类别：网络参数、关机参数、日志参数、系统集成、通用参数
  - 新增 5 个实用示例（涵盖常见场景）
  - 环境变量分类展示，便于查找
  - 参数说明更加清晰简洁

- **布尔参数解析增强** 🔧
  - 新增 `parse_bool_arg()` 统一解析函数
  - 支持长选项格式：`--dry-run=no`, `--timestamp=yes`
  - 支持短选项格式：`-dno`, `-Tyes` (值必须直接连接)
  - 增强 `get_env_bool()` 支持所有布尔格式

### Removed
- **清理向后兼容代码** 🧹
  - 移除 `OPENUPS_NO_TIMESTAMP` 环境变量支持（使用 `OPENUPS_TIMESTAMP` 替代）
  - 代码简化 3 行，消除历史包袱
  - 项目独立维护，无需向后兼容性负担

### Fixed
- 布尔参数解析完全统一，所有格式一致处理
- 短选项可选参数语法完全符合 POSIX 规范
- `get_env_bool()` 函数现在完全支持 yes/no, on/off 格式

### Testing
- ✅ 80+ 综合测试用例全部通过
  - 51 个自动化参数测试
  - 10 个真实场景测试
  - 11 个边界条件测试
  - 8 个错误处理测试
- ✅ 所有参数组合验证完成
- ✅ 环境变量优先级测试通过
- ✅ 布尔格式兼容性验证

### Documentation
- 📝 更新所有文档至 v1.2.0
- 📝 README.md: 重新组织配置表格，增加参数分类
- 📝 QUICKSTART.md: 更新所有示例为新参数格式
- 📝 TECHNICAL.md: 更新技术细节和架构说明
- 📝 systemd/openups.service: 更新环境变量配置示例
- 📝 .github/copilot-instructions.md: 同步所有变更

### Performance
- 📊 二进制大小维持：**39 KB**
- 📊 内存占用维持：**< 5 MB**
- 📊 安全评分维持：**10/10** 🏆
- 📊 编译警告：**0** ✅

---

## [1.1.1] - 2025-11-04

### Changed
- **CLI 参数系统优化**: 完善短选项支持
  - 所有长选项现在都支持对应的短选项
  - 短选项列表：`-t -i -n -w -s -r -S -D -C -P -L -6 -d -N -Y -T -M -W -Z -h`

### Fixed
- **GCC 13 兼容性**: 为 stdckdint.h (C23) 添加后备实现

---

## [1.1.0] - 2025-10-27

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
- **语言**：C11 标准 | **代码量**：1685 行 | **二进制**：45 KB
- **安全**：10/10 | **依赖**：零第三方

---
