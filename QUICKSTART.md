# OpenUPS - 快速开始指南

## 🚀 5 分钟快速上手

### 1. 编译项目
```bash
cd /home/light/github/openups
make
```

### 2. 查看帮助
```bash
./bin/openups --help
```

### 3. 测试运行（需要 root 权限）
```bash
# Dry-run 模式（默认，不会真正关机）
sudo ./bin/openups --target 8.8.8.8 --interval 5 --threshold 3 --dry-run

# 实际运行模式（小心使用！）
sudo ./bin/openups --target 8.8.8.8 --interval 5 --threshold 3 --no-dry-run
```

---

## 📖 常用场景

### 场景 1: 监控互联网连接
```bash
# 监控 Cloudflare DNS，10 秒间隔，失败 5 次触发
sudo ./bin/openups \
  --target 1.1.1.1 \
  --interval 10 \
  --threshold 5 \
  --timeout 2000 \
  --log-level debug
```

### 场景 2: 监控 IPv6 连接
```bash
# 监控 Google DNS IPv6
sudo ./bin/openups \
  --target 2001:4860:4860::8888 \
  --ipv6 \
  --interval 5 \
  --threshold 3
```

### 场景 3: 延迟关机模式
```bash
# 失败后延迟 5 分钟关机
sudo ./bin/openups \
  --target 8.8.8.8 \
  --shutdown-mode delayed \
  --delay-minutes 5 \
  --no-dry-run
```

### 场景 4: 仅记录日志（不关机）
```bash
# 仅记录失败，持续监控
sudo ./bin/openups \
  --target 192.168.1.1 \
  --shutdown-mode log-only \
  --syslog
```

### 场景 5: 自定义脚本
```bash
# 创建自定义脚本
cat > /tmp/my-script.sh << 'EOF'
#!/bin/bash
echo "Network failed at $(date)" >> /var/log/network-failure.log
# 发送通知、备份数据等
EOF
chmod +x /tmp/my-script.sh

# 使用自定义脚本
sudo ./bin/openups \
  --target 8.8.8.8 \
  --custom-script /tmp/my-script.sh \
  --shutdown-mode custom \
  --no-dry-run
```

---

## 🔧 systemd 服务部署

### 1. 安装为系统服务
```bash
sudo ./systemd/install.sh
```

### 2. 配置服务（编辑环境变量）
```bash
sudo systemctl edit openups
```

添加配置：
```ini
[Service]
Environment="OPENUPS_TARGET=8.8.8.8"
Environment="OPENUPS_INTERVAL=10"
Environment="OPENUPS_THRESHOLD=5"
Environment="OPENUPS_DRY_RUN=false"
```

### 3. 启动服务
```bash
sudo systemctl start openups
sudo systemctl status openups
```

### 4. 查看日志
```bash
sudo journalctl -u openups -f
```

### 5. 开机自启
```bash
sudo systemctl enable openups
```

---

## 🔍 故障排查

### 问题 1: "Operation not permitted"
**原因**: 需要 root 权限或 CAP_NET_RAW 能力

**解决**:
```bash
# 方法 1: 使用 sudo
sudo ./bin/openups ...

# 方法 2: 添加 CAP_NET_RAW 能力（推荐）
sudo setcap cap_net_raw+ep ./bin/openups
./bin/openups ...  # 现在可以不用 sudo
```

### 问题 2: 找不到目标主机
**原因**: DNS 解析失败或网络不可达

**解决**:
```bash
# 先测试 DNS 解析
nslookup 目标主机

# 直接使用 IP 地址
sudo ./bin/openups --target 8.8.8.8
```

### 问题 3: systemd 服务启动失败
**原因**: 权限不足或配置错误

**解决**:
```bash
# 查看详细错误
sudo journalctl -xe -u openups

# 检查服务文件
sudo systemctl cat openups

# 验证配置
sudo /usr/local/bin/openups --help
```

---

## 📊 监控和统计

### 查看实时统计信息
在运行过程中发送 `SIGUSR1` 信号：
```bash
# 查找进程 PID
ps aux | grep openups

# 发送信号
sudo kill -SIGUSR1 <PID>

# 查看日志中的统计信息
sudo journalctl -u openups | tail -20
```

输出示例：
```
[INFO] Statistics: total=150 successful=148 failed=2 success_rate=98.67% min_latency=0.23ms max_latency=15.42ms avg_latency=1.85ms uptime=1500s
```

---

## 🔒 安全最佳实践

### 1. 使用 CAP_NET_RAW 而非 root
```bash
# 编译后设置能力
sudo setcap cap_net_raw+ep /usr/local/bin/openups

# 以普通用户运行
/usr/local/bin/openups --target 8.8.8.8
```

### 2. 始终先在 dry-run 模式测试
```bash
# 测试配置
sudo ./bin/openups --target 8.8.8.8 --threshold 2 --dry-run

# 确认无误后再启用实际关机
sudo ./bin/openups --target 8.8.8.8 --threshold 2 --no-dry-run
```

### 3. 使用 systemd 管理
```bash
# systemd 提供自动重启、日志记录等功能
sudo systemctl enable --now openups
```

### 4. 监控日志
```bash
# 定期检查日志
sudo journalctl -u openups --since "1 hour ago"

# 设置日志告警（可选）
sudo journalctl -u openups -f | grep -i error
```

---

## 🧪 验证安装

运行测试套件：
```bash
cd /home/light/github/openups
./test.sh
```

预期输出：
```
========================================
OpenUPS 自动化测试
========================================

[1/10] 编译检查...                ✓
[2/10] 帮助信息...                ✓
[3/10] 版本信息...                ✓
...
[10/10] 代码质量检查...           ✓

========================================
✓ 所有测试通过！
========================================
```

---

## 📚 相关文档

| 文档 | 说明 |
|------|------|
| 📖 [README.md](README.md) | 项目概览和特性介绍 |
| 🔧 [TECHNICAL.md](TECHNICAL.md) | 架构设计和开发指南 |
| 🤝 [CONTRIBUTING.md](CONTRIBUTING.md) | 如何参与贡献 |
| 📋 [CHANGELOG.md](CHANGELOG.md) | 版本更新历史 |

---

## 💡 提示和技巧

### 调整日志级别
```bash
# 调试模式（详细日志）
sudo ./bin/openups --target 8.8.8.8 --log-level debug

# 安静模式（仅错误）
```bash
# 安静模式（仅警告和错误）
sudo ./bin/openups --target 8.8.8.8 --log-level warn
```
```

### 自定义 ping 参数
```bash
# 调整超时和包大小
sudo ./bin/openups \
  --target 8.8.8.8 \
  --timeout 5000 \
  --packet-size 128 \
  --retries 3
```

### 环境变量配置
```bash
# 使用环境变量
export OPENUPS_TARGET=8.8.8.8
export OPENUPS_INTERVAL=10
export OPENUPS_THRESHOLD=5
export OPENUPS_DRY_RUN=false

sudo -E ./bin/openups
```

---

**需要帮助？** 查看 [GitHub Issues](https://github.com/surpasslight12/openups/issues) 或阅读完整文档。
