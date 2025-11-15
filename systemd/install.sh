#!/usr/bin/env bash
# 安装 OpenUPS 服务

set -euo pipefail

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# 检查是否为 root
if [[ $EUID -ne 0 ]]; then
   echo -e "${RED}错误: 此脚本需要 root 权限${NC}" 
   echo "请使用 sudo 运行: sudo $0"
   exit 1
fi

# 检查二进制文件是否存在
if [[ ! -f "bin/openups" ]]; then
    echo -e "${RED}错误: 找不到 bin/openups${NC}"
    echo "请先运行 'make' 编译项目"
    exit 1
fi

echo -e "${GREEN}=== OpenUPS 服务安装 ===${NC}"
echo ""

# 复制二进制文件
echo "复制二进制文件到 /usr/local/bin/openups..."
cp bin/openups /usr/local/bin/openups
chmod 755 /usr/local/bin/openups

# 设置 capability
echo "设置 CAP_NET_RAW capability..."
setcap cap_net_raw+ep /usr/local/bin/openups || true

# 复制 systemd 服务文件
echo "安装 systemd 服务文件..."
cp systemd/openups.service /etc/systemd/system/openups.service

# 创建日志目录并设置权限（与服务 ReadWritePaths 一致）
mkdir -p /var/log/openups
chown root:root /var/log/openups
chmod 0755 /var/log/openups

# 重新加载 systemd
echo "重新加载 systemd..."
systemctl daemon-reload

# 启用服务
echo "启用 openups.service..."
systemctl enable openups.service

echo ""
echo -e "${GREEN}=== 安装完成 ===${NC}"
echo ""
echo "使用以下命令管理服务："
echo "  启动: sudo systemctl start openups.service"
echo "  停止: sudo systemctl stop openups.service"
echo "  状态: sudo systemctl status openups.service"
echo "  日志: sudo journalctl -u openups.service -f"
echo ""
echo -e "${YELLOW}注意: 当前配置为 dry-run 模式，不会真实关机${NC}"
echo "编辑 /etc/systemd/system/openups.service 修改配置"
