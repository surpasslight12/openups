# OpenUPS 贡献指南

欢迎贡献！本指南帮助你快速参与项目。

## 快速开始

### 环境要求
- **编译器**: GCC 14.0+ 或 Clang 15.0+（C23 支持）
- **系统**: Linux（需要 CAP_NET_RAW 权限）
- **工具**: make, git

### 设置开发环境
```bash
git clone https://github.com/surpasslight12/openups.git
cd openups
make clean && make
./test.sh
```

## 报告问题

### Bug 报告
提供环境信息、问题描述、复现步骤和日志输出（`--log-level debug`）

### 功能请求
清晰描述需求场景和使用示例

## 提交代码

### 开发流程
```bash
git checkout -b feature/your-feature
make clean && make && ./test.sh
git commit -m "feat: description"
git push origin feature/your-feature
```

### 代码规范
```c
// 类型: 小写_下划线_t
typedef struct { ... } config_t;

// 函数: module_action
void logger_init(...);

// 使用 snprintf
snprintf(buf, sizeof(buf), "%s", str);
```

### Commit 格式
`<type>: <description>` - type 可选: feat, fix, docs, style, refactor, test, chore

### PR 检查清单
- [ ] 编译通过（0 警告）
- [ ] 测试通过
- [ ] 更新文档

## 开发参考

- **详细指南**: [DEVELOPMENT.md](docs/DEVELOPMENT.md)
- **架构设计**: [ARCHITECTURE.md](docs/ARCHITECTURE.md)
- **调试**: `gdb --args ./bin/openups...`

## 社区准则

尊重所有贡献者，建设性反馈，专注技术讨论

---

**许可证**: 贡献代码将在 MIT 许可证下发布  
**维护**: OpenUPS 项目团队  
**更新**: 2025-11-01
