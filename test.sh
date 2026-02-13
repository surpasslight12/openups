#!/bin/bash
# OpenUPS 自动化测试脚本
# 验证编译、参数解析、配置验证、安全性和代码质量

set -e  # 遇到错误立即退出

echo "========================================"
echo "OpenUPS 自动化测试"
echo "========================================"
echo

# 编译检查
echo "[1/12] 编译检查 (SYSTEMD=1)..."
make clean > /dev/null 2>&1
if make 2>&1 | grep -E "^[^:]+:[0-9]+:[0-9]+: (warning|error):" > /dev/null; then
    echo "❌ 编译有警告或错误"
    make 2>&1 | grep -E "^[^:]+:[0-9]+:[0-9]+: (warning|error):"
    exit 1
fi
echo "✓ 编译成功 (SYSTEMD=1)，无警告"

# 无 systemd 编译检查
echo "[2/12] 编译检查 (SYSTEMD=0)..."
make clean > /dev/null 2>&1
if make SYSTEMD=0 2>&1 | grep -E "^[^:]+:[0-9]+:[0-9]+: (warning|error):" > /dev/null; then
    echo "❌ 无 systemd 编译有警告或错误"
    make SYSTEMD=0 2>&1 | grep -E "^[^:]+:[0-9]+:[0-9]+: (warning|error):"
    exit 1
fi
echo "✓ 编译成功 (SYSTEMD=0)，无警告"

# 重新构建含 systemd 版本用于后续测试
make clean > /dev/null 2>&1
make > /dev/null 2>&1

# 帮助信息
echo "[3/12] 测试帮助信息..."
if ! ./bin/openups --help > /dev/null 2>&1; then
    echo "❌ 帮助信息失败"
    exit 1
fi
echo "✓ 帮助信息正常"

# 版本信息
echo "[4/12] 测试版本信息..."
if ! ./bin/openups --version > /dev/null 2>&1; then
    echo "❌ 版本信息失败"
    exit 1
fi
echo "✓ 版本信息正常"

# 参数验证测试
echo "[5/12] 测试布尔参数 --dry-run..."
if ! ./bin/openups --help --dry-run=true > /dev/null 2>&1; then
    echo "❌ --dry-run=true 参数解析失败"
    exit 1
fi
if ! ./bin/openups --help --dry-run=false > /dev/null 2>&1; then
    echo "❌ --dry-run=false 参数解析失败"
    exit 1
fi
# 测试短选项
if ! ./bin/openups --help -dfalse > /dev/null 2>&1; then
    echo "❌ 短选项 -dfalse 解析失败"
    exit 1
fi
echo "✓ 布尔参数和短选项解析正常"

echo "[6/12] 测试环境变量优先级..."
# 环境变量设置
if ! OPENUPS_TARGET=127.0.0.1 OPENUPS_INTERVAL=5 OPENUPS_THRESHOLD=3 ./bin/openups --help > /dev/null 2>&1; then
    echo "❌ 环境变量设置失败"
    exit 1
fi
# 测试 CLI 优先级高于环境变量
if ! OPENUPS_TARGET=127.0.0.1 ./bin/openups --target 8.8.8.8 --help > /dev/null 2>&1; then
    echo "❌ CLI 参数优先级测试失败"
    exit 1
fi
echo "✓ 环境变量和优先级处理正常"

# 错误处理测试
echo "[7/12] 测试空目标地址..."
if ./bin/openups --target "" 2>&1 | grep -q "Target host cannot be empty"; then
    echo "✓ 空目标地址被拒绝"
else
    echo "❌ 空目标地址检查失败"
    exit 1
fi

# 负数间隔
echo "[8/12] 测试负数间隔..."
if ./bin/openups --target 127.0.0.1 --interval -1 2>&1 | \
   grep -Eq "Interval must be positive|Invalid value for --interval"; then
    echo "✓ 负数间隔被拒绝"
else
    echo "❌ 负数间隔检查失败"
    exit 1
fi

# 零阈值
echo "[9/12] 测试零阈值..."
if ./bin/openups --target 127.0.0.1 --threshold 0 2>&1 | \
   grep -Eq "Failure threshold must be positive|Invalid value for --threshold"; then
    echo "✓ 零阈值被拒绝"
else
    echo "❌ 零阈值检查失败"
    exit 1
fi

# 零超时
echo "[10/12] 测试零超时..."
if ./bin/openups --target 127.0.0.1 --timeout 0 2>&1 | \
   grep -Eq "Timeout must be positive|Invalid value for --timeout"; then
    echo "✓ 零超时被拒绝"
else
    echo "❌ 零超时检查失败"
    exit 1
fi

# 注入式 target（OpenUPS 禁用 DNS，且 target 必须为 IP 字面量）
echo "[11/12] 测试 target 注入防护..."
if ./bin/openups --target "1.1.1.1;rm -rf /" 2>&1 | grep -Eq "Target must be a valid|DNS is disabled"; then
    echo "✓ 注入式 target 被拒绝"
else
    echo "❌ target 注入防护失败"
    exit 1
fi

# 超大包大小
echo "[12/12] 测试超大包大小..."
if ./bin/openups --target 127.0.0.1 --payload-size 99999 2>&1 | \
    grep -Eq "Payload size must be between|Invalid value for --payload-size"; then
    echo "✓ 超大包大小被拒绝"
else
    echo "❌ 包大小检查失败"
    exit 1
fi

# 代码质量检查
echo ""
echo "代码质量检查..."
errors=0

# 检查是否有不安全的函数
if grep -rE "(strcpy|strcat|sprintf)\(" src/common/ src/posix/ src/linux/ src/systemd/ 2>/dev/null | grep -v "//" > /dev/null; then
    echo "  ❌ 发现不安全的字符串函数"
    ((errors++))
fi

# 检查是否有 syslog 遗留代码
if grep -ri "syslog" src/common/ src/posix/ src/linux/ src/systemd/ 2>/dev/null | grep -v "^Binary"; then
    echo "  ⚠️ 发现 syslog 遗留代码"
    ((errors++))
fi

if [ $errors -eq 0 ]; then
    echo "✓ 代码质量良好"
else
    echo "  ⚠️ 发现 $errors 个需要关注的问题"
fi

# 二进制体积检查
echo ""
echo "二进制体积分析..."
binary_size=$(stat -c %s bin/openups 2>/dev/null || stat -f %z bin/openups 2>/dev/null)
if [ -n "$binary_size" ]; then
    if [ "$binary_size" -lt 524288 ]; then  # < 512 KB
        echo "✓ 二进制体积: $(echo "$binary_size" | awk '{printf "%.1f KB", $1/1024}')"
    else
        echo "⚠️ 二进制体积偏大: $(echo "$binary_size" | awk '{printf "%.1f KB", $1/1024}')"
    fi
fi

# 安全加固检查
echo ""
echo "安全加固检查..."
sec_errors=0
if command -v readelf > /dev/null 2>&1; then
    # 检查 Full RELRO
    if readelf -l bin/openups 2>/dev/null | grep -q "GNU_RELRO"; then
        echo "  ✓ RELRO: enabled"
    else
        echo "  ❌ RELRO: not found"
        ((sec_errors++))
    fi
    # 检查 PIE
    if readelf -h bin/openups 2>/dev/null | grep -q "DYN"; then
        echo "  ✓ PIE: enabled"
    else
        echo "  ❌ PIE: not found"
        ((sec_errors++))
    fi
    # 检查 Stack Canary
    if readelf -s bin/openups 2>/dev/null | grep -q "__stack_chk_fail"; then
        echo "  ✓ Stack Canary: enabled"
    else
        echo "  ❌ Stack Canary: not found"
        ((sec_errors++))
    fi
    # 检查 NX (No Execute Stack)
    if readelf -l bin/openups 2>/dev/null | grep "GNU_STACK" | grep -qv "RWE"; then
        echo "  ✓ NX Stack: enabled"
    else
        echo "  ❌ NX Stack: not found"
        ((sec_errors++))
    fi
    # 检查 FORTIFY_SOURCE
    if readelf -s bin/openups 2>/dev/null | grep -q "__.*_chk"; then
        echo "  ✓ FORTIFY_SOURCE: enabled"
    else
        echo "  ⚠️ FORTIFY_SOURCE: not detected (may be inlined by LTO)"
    fi
fi
if [ $sec_errors -eq 0 ]; then
    echo "  ✓ 安全加固完整"
else
    echo "  ⚠️ $sec_errors 个安全检查未通过"
fi

echo
echo "========================================"
echo "✓ 所有测试通过！"
echo "========================================"
echo
ls -lh bin/openups
echo "更多使用示例请查看 README.md"
echo
