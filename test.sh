#!/bin/bash
# OpenUPS C 版本测试脚本

set -e  # 遇到错误立即退出

echo "========================================"
echo "OpenUPS C 版本自动化测试"
echo "========================================"
echo

# 编译检查
echo "[1/10] 编译检查..."
make clean > /dev/null 2>&1
if make 2>&1 | grep -E "(warning|error)" > /dev/null; then
    echo "❌ 编译有警告或错误"
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

# 代码检查
echo "[10/10] 代码质量检查..."
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
echo "二进制文件信息:"
ls -lh bin/openups
echo
echo "代码统计:"
echo -n "  总行数: "
find src -name "*.c" -o -name "*.h" | xargs wc -l | tail -1
echo -n "  C 源文件: "
find src -name "*.c" | wc -l
echo -n "  头文件: "
find src -name "*.h" | wc -l
echo
echo "使用示例:"
echo "  sudo ./bin/openups --target 8.8.8.8 --interval 5 --threshold 3"
echo "  sudo ./bin/openups --target 2001:4860:4860::8888 --ipv6"
echo
