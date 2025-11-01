# OpenUPS - 文档目录

本目录包含 OpenUPS 项目的技术深度文档。

## 📚 文档导航

### 🚀 快速开始（推荐顺序）
1. **[../README.md](../README.md)** - 项目主页，了解项目特性
2. **[../QUICKSTART.md](../QUICKSTART.md)** - 5 分钟快速上手
3. **[ARCHITECTURE.md](ARCHITECTURE.md)** - 深入理解架构（可选）

### 📖 根目录文档（用户导向）
- **[README.md](../README.md)** - 项目主页和特性介绍
- **[QUICKSTART.md](../QUICKSTART.md)** - 快速上手和常用场景
- **[CONTRIBUTING.md](../CONTRIBUTING.md)** - 贡献指南（开发者）
- **[CHANGELOG.md](../CHANGELOG.md)** - 版本更新日志

### 🏗️ 技术文档（docs/ 目录）
- **[ARCHITECTURE.md](ARCHITECTURE.md)** - 架构设计详解
  - 模块化设计和职责
  - 依赖关系和数据流
  - 关键实现细节
  - 扩展开发指南

- **[CODE_REVIEW.md](CODE_REVIEW.md)** - 代码审查报告 🆕
  - C23 标准合规性分析
  - 安全性和性能评估
  - 发现问题和修复记录
  - 代码质量度量

- **[RUNTIME_ANALYSIS.md](RUNTIME_ANALYSIS.md)** - 运行时逻辑分析 🆕
  - 程序生命周期流程
  - 状态机和失败处理逻辑
  - 并发安全性验证
  - 实际运行测试结果

### 🤖 AI 协作文档
- **[../.github/copilot-instructions.md](../.github/copilot-instructions.md)** - AI 编码助手指导
- **[../.github/DOCUMENTATION.md](../.github/DOCUMENTATION.md)** - 文档组织规范
- **[../.cursorrules](../.cursorrules)** - Cursor AI 编码规范

### 📦 归档文档
- **[archive/](archive/)** - 历史版本和临时报告
  - `PROJECT_STATUS.md` - 已归档的项目状态报告
  - `C23_IMPROVEMENTS.md` - C23 改进技术报告

## 🎯 如何选择文档

| 你的角色 | 推荐阅读 |
|---------|---------|
| 🆕 新用户 | README.md → QUICKSTART.md |
| 👨‍💻 开发者 | CONTRIBUTING.md → ARCHITECTURE.md → CODE_REVIEW.md |
| 🔧 运维人员 | QUICKSTART.md → ARCHITECTURE.md |
| 🤖 AI 助手 | copilot-instructions.md → RUNTIME_ANALYSIS.md |
| 📚 文档维护者 | DOCUMENTATION.md |
| 🔍 代码审查者 | CODE_REVIEW.md → RUNTIME_ANALYSIS.md |

## 📝 文档贡献

### 更新现有文档
1. 遵循 [DOCUMENTATION.md](../.github/DOCUMENTATION.md) 规范
2. 确保示例代码可运行
3. 更新文档版本号和日期

### 创建新文档
1. **必须先阅读** [DOCUMENTATION.md](../.github/DOCUMENTATION.md)
2. 检查内容是否与现有文档重复
3. 确定正确的文档位置
4. 创建后更新本索引文件

---

**维护**: OpenUPS 项目团队  
**更新**: 2025-11-01  
**版本**: v1.1.0
