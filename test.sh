#!/bin/bash
# OpenUPS 测试脚本

set -e  # 遇到错误立即退出

echo "========================================"
echo "OpenUPS 自动化测试"
echo "========================================"
echo

# 编译检查
echo "[1/10] 编译检查..."
make clean > /dev/null 2>&1
# 只检查实际的警告和错误，忽略编译器输出中的其他信息
if make 2>&1 | grep -E "^[^:]+:[0-9]+:[0-9]+: (warning|error):" > /dev/null; then
    echo "❌ 编译有警告或错误"
    make 2>&1 | grep -E "^[^:]+:[0-9]+:[0-9]+: (warning|error):"
    exit 1
fi
echo "✓ 编译成功，无警告"

# 帮助信息
echo "[2/10] 测试帮助信息..."
if ! ./bin/openups --help > /dev/null 2>&1; then
    echo "❌ 帮助信息失败"
    exit 1
fi
echo "✓ 帮助信息正常"

# 版本信息
echo "[3/10] 测试版本信息..."
if ! ./bin/openups --version > /dev/null 2>&1; then
    echo "❌ 版本信息失败"
    exit 1
fi
echo "✓ 版本信息正常"

# 空目标地址
echo "[4/10] 测试空目标地址..."
if ./bin/openups --target "" 2>&1 | grep -q "Target host cannot be empty"; then
    echo "✓ 空目标地址被拒绝"
else
    echo "❌ 空目标地址检查失败"
    exit 1
fi

# 负数间隔
echo "[5/10] 测试负数间隔..."
if ./bin/openups --target 127.0.0.1 --interval -1 2>&1 | grep -q "Interval must be positive"; then
    echo "✓ 负数间隔被拒绝"
else
    echo "❌ 负数间隔检查失败"
    exit 1
fi

# 零阈值
echo "[6/10] 测试零阈值..."
if ./bin/openups --target 127.0.0.1 --threshold 0 2>&1 | grep -q "Failure threshold must be positive"; then
    echo "✓ 零阈值被拒绝"
else
    echo "❌ 零阈值检查失败"
    exit 1
fi

# 零超时
echo "[7/10] 测试零超时..."
if ./bin/openups --target 127.0.0.1 --timeout 0 2>&1 | grep -q "Timeout must be positive"; then
    echo "✓ 零超时被拒绝"
else
    echo "❌ 零超时检查失败"
    exit 1
fi

# 危险的自定义脚本路径
echo "[8/10] 测试危险路径注入..."
if ./bin/openups --custom-script "/tmp/test;rm -rf /" 2>&1 | grep -q "unsafe characters"; then
    echo "✓ 危险路径被拒绝"
else
    echo "❌ 路径注入防护失败"
    exit 1
fi

# 超大包大小
echo "[9/10] 测试超大包大小..."
if ./bin/openups --target 127.0.0.1 --packet-size 99999 2>&1 | grep -q "Packet size must be between"; then
    echo "✓ 超大包大小被拒绝"
else
    echo "❌ 包大小检查失败"
    exit 1
fi

# Syslog 参数解析
echo "[10/12] 测试 syslog 参数..."
if ./bin/openups --syslog=yes --help > /dev/null 2>&1; then
    echo "✓ syslog=yes 参数解析正常"
else
    echo "❌ syslog=yes 参数解析失败"
    exit 1
fi

if ./bin/openups --syslog=no --help > /dev/null 2>&1; then
    echo "✓ syslog=no 参数解析正常"
else
    echo "❌ syslog=no 参数解析失败"
    exit 1
fi

# 环境变量 syslog 支持
echo "[11/12] 测试 syslog 环境变量..."
if OPENUPS_SYSLOG=yes ./bin/openups --help > /dev/null 2>&1; then
    echo "✓ OPENUPS_SYSLOG=yes 环境变量正常"
else
    echo "❌ OPENUPS_SYSLOG=yes 环境变量失败"
    exit 1
fi

# 代码检查
echo "[12/12] 代码质量检查..."
errors=0

# 检查是否有未使用的变量（简单检查）
if grep -r "unused variable" . --include="*.c" 2>/dev/null | grep -v test.sh > /dev/null; then
    echo "  警告: 发现未使用的变量"
    ((errors++))
fi

# 检查是否有不安全的函数
if grep -E "(strcpy|strcat|sprintf)\(" src/*.c 2>/dev/null | grep -v "//" > /dev/null; then
    echo "  警告: 发现不安全的字符串函数"
    ((errors++))
fi

if [ $errors -eq 0 ]; then
    echo "✓ 代码质量良好"
else
    echo "  注意: 发现 $errors 个潜在问题"
fi

echo
echo "========================================"
echo "✓ 所有测试通过！"
echo "========================================"
echo
ls -lh bin/openups
echo "更多使用示例请查看 QUICKSTART.md"
echo
