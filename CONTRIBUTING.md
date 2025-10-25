# OpenUPS C 贡献指南

**版本**: v1.1.0  
**欢迎贡献！** 🎉

感谢你对 OpenUPS C 项目的关注。本文档将帮助你了解如何为项目做出贡献。

- 🐛 报告 bug
- 💡 提出新功能建议
- 📝 改进文档
- 🔧 提交代码补丁
- 🧪 添加测试用例

---

## 开始之前

1. **检查现有 Issue**：在创建新 Issue 前，请先搜索是否已有相关讨论
2. **Fork 仓库**：从主分支创建您自己的 fork
3. **阅读文档**：熟悉 [架构设计](docs/ARCHITECTURE.md) 和 [README](README.md)

---

## 报告 Bug

### 创建 Bug Report

提供以下信息：

```markdown
**环境**
- OS: Linux (发行版和版本)
- Kernel: uname -r 输出
- Compiler: gcc --version (需要 GCC 14.0+ 或 Clang 15.0+)
- OpenUPS 版本: ./bin/openups --version
- C 标准: C23 (C2x)

**问题描述**
清晰描述遇到的问题

**复现步骤**
1. 执行命令: ./bin/openups --target ...
2. 观察到: ...
3. 预期行为: ...

**日志输出**
```
粘贴相关日志（使用 --log-level debug）
```

**额外信息**
其他可能有帮助的信息
```

---

## 提交代码

### 代码规范

#### 命名规范

```c
// 类型名：小写字母 + 下划线 + _t 后缀
typedef struct {
    int value;
} my_type_t;

// 函数名：模块名_功能描述
void logger_init(logger_t* logger);
bool config_validate(const config_t* config);

// 变量名：小写字母 + 下划线
int consecutive_fails;
char error_msg[256];

// 宏定义：大写字母 + 下划线
#define MAX_BUFFER_SIZE 1024
#define PROGRAM_NAME "openups"

// 枚举：大写字母 + 下划线
typedef enum {
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_INFO
} log_level_t;
```

#### 代码风格

```c
// 1. 缩进：4 空格
void function(void) {
    if (condition) {
        do_something();
    }
}

// 2. 大括号：K&R 风格
if (condition) {
    ...
} else {
    ...
}

// 3. 指针：靠近类型
int* ptr;
const char* str;

// 4. 单行长度：<= 100 字符
// 5. 函数长度：尽量 <= 50 行

// 6. 头文件保护
#ifndef OPENUPS_MODULE_H
#define OPENUPS_MODULE_H
...
#endif /* OPENUPS_MODULE_H */
```

#### 注释规范

```c
/**
 * 执行 ICMP ping 操作
 * 
 * @param pinger ICMP pinger 实例
 * @param target 目标主机或 IP 地址
 * @param timeout_ms 超时时间（毫秒）
 * @param packet_size ICMP payload 大小（字节）
 * @return ping 结果（成功状态和延迟时间）
 */
ping_result_t icmp_pinger_ping(icmp_pinger_t* pinger, 
                               const char* target,
                               int timeout_ms, 
                               int packet_size);

/* 行内注释：解释 WHY 而非 WHAT */
int attempts = 3; /* 重试 3 次以减少误判 */

/* 复杂逻辑：添加块注释 */
/* 
 * 计算 ICMP 校验和（RFC 1071）
 * 1. 将数据按 16 位累加
 * 2. 如果有进位则回卷
 */
```

### 开发流程

#### 1. 创建分支

```bash
git checkout -b feature/your-feature-name
# 或
git checkout -b fix/bug-description
```

#### 2. 开发和测试

```bash
# 编译
make

# 测试
./bin/openups --target 127.0.0.1 --interval 1 --threshold 2 --dry-run --verbose

# 调试版本
make clean
make CC=gcc CFLAGS="-g -O0 -std=c11 -Wall -Wextra"
gdb --args ./bin/openups --target 127.0.0.1 --log-level debug
```

#### 3. 提交代码

```bash
# 检查代码风格（如果有工具）
# clang-format -i src/*.c src/*.h

# 添加文件
git add .

# 提交（使用清晰的提交信息）
git commit -m "feat: add IPv6 support for ICMP ping"
# 或
git commit -m "fix: resolve buffer overflow in config parsing"
```

**提交信息格式**：
- `feat:` 新功能
- `fix:` Bug 修复
- `docs:` 文档更新
- `style:` 代码格式（不影响功能）
- `refactor:` 重构
- `perf:` 性能优化
- `test:` 添加测试
- `chore:` 构建/工具链更新

#### 4. 推送和创建 Pull Request

```bash
git push origin feature/your-feature-name
```

然后在 GitHub 上创建 Pull Request，提供：

```markdown
**改动描述**
简要说明此 PR 的目的和改动内容。

**关联 Issue**
Closes #123

**改动类型**
- [ ] Bug 修复
- [ ] 新功能
- [ ] 重构
- [ ] 文档更新
- [ ] 性能优化

**测试验证**
- [ ] 编译通过（无警告）
- [ ] 手动测试通过
- [ ] 添加了测试用例（如适用）
- [ ] 更新了文档（如适用）

**截图/日志**
如适用，添加测试截图或日志输出。
```

---

## 目录结构约定

```
src/
├── *.h           # 头文件（接口声明）
├── *.c           # 实现文件
└── main.c        # 程序入口（保持简洁）

systemd/
├── *.service     # systemd 服务文件
├── *.sh          # 安装/卸载脚本

docs/
├── *.md          # 文档（统一大写命名）
└── README.md     # 文档索引

根目录
├── Makefile      # 主构建系统
├── CHANGELOG.md  # 变更日志
├── README.md     # 项目说明
├── LICENSE       # MIT 许可证
├── CONTRIBUTING.md # 本文件
└── .gitignore    # Git 忽略文件
```

---

## 文档贡献

### 文档规范

```markdown
# 标题层级：最多 3 层

## 二级标题

### 三级标题

**代码块**：指定语言

```c
/* C 代码 */
void example(void) {}
```

```bash
# Bash 命令
sudo systemctl start openups.service
```

**表格**：对齐清晰

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| --target | string | 1.1.1.1 | 目标主机 |

**链接**：使用相对路径

[架构设计](docs/ARCHITECTURE.md)
```

---

## 获取帮助

- **Issue**: 提出问题或讨论
- **Email**: 联系维护者
- **Documentation**: 查阅 [docs/](docs/) 目录

---

## 致谢

感谢所有贡献者的付出！

---

**注意**：
- 所有贡献必须遵守 [MIT 许可证](LICENSE)
- 提交代码即表示您同意将代码授权给项目使用
- 请尊重他人，保持友好和专业的态度
