#!/usr/bin/env bash
# 卸载 OpenUPS 服务

set -euo pipefail

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m' # No Color

# 检查是否为 root
if [[ $EUID -ne 0 ]]; then
   echo -e "${RED}错误: 此脚本需要 root 权限${NC}" 
   echo "请使用 sudo 运行: sudo $0"
   exit 1
fi

echo -e "${GREEN}=== OpenUPS 服务卸载 ===${NC}"
echo ""

# 停止并禁用服务
if systemctl is-active --quiet openups.service; then
    echo "停止 openups.service..."
    systemctl stop openups.service
fi

if systemctl is-enabled --quiet openups.service; then
    echo "禁用 openups.service..."
    systemctl disable openups.service
fi

# 删除服务文件
if [[ -f /etc/systemd/system/openups.service ]]; then
    echo "删除服务文件..."
    rm -f /etc/systemd/system/openups.service
fi

# 重新加载 systemd
echo "重新加载 systemd..."
systemctl daemon-reload

# 删除二进制文件（可选）
read -p "是否删除二进制文件 /usr/local/bin/openups? (y/n) " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    echo "删除二进制文件..."
    rm -f /usr/local/bin/openups
fi

echo ""
echo -e "${GREEN}=== 卸载完成 ===${NC}"
