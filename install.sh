#!/usr/bin/env bash
# OpenUPS Installation & Management Script

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN_PATH="/usr/local/bin/openups"
SERVICE_NAME="openups.service"
SERVICE_PATH="/etc/systemd/system/${SERVICE_NAME}"
ENV_FILE="/etc/default/openups"

print_step() { echo -e "${CYAN}==>${NC} ${GREEN}$1${NC}"; }
print_warn() { echo -e "${YELLOW}WARNING:${NC} $1"; }
print_err() { echo -e "${RED}ERROR:${NC} $1"; }

check_root() {
    if [[ $EUID -ne 0 ]]; then
        print_err "This script must be run as root (or with sudo)"
        exit 1
    fi
}

build_project() {
    print_step "Building OpenUPS from source..."
    cd "$PROJECT_DIR"
    if ! make clean; then
        print_err "Failed to clean previous build"
        exit 1
    fi
    if ! make; then
        print_err "Build failed. Do you have gcc and make installed?"
        exit 1
    fi
    print_step "Build successful."
}

configure_env() {
    print_step "Configuring parameters (interactive)"
    
    echo -e "Press Enter to accept the defaults.\n"

    read -p "Target IP to monitor [1.1.1.1]: " TARGET
    TARGET=${TARGET:-1.1.1.1}

    read -p "Check Interval in seconds [10]: " INTERVAL
    INTERVAL=${INTERVAL:-10}

    read -p "Failure Threshold (number of pings) [5]: " THRESHOLD
    THRESHOLD=${THRESHOLD:-5}

    read -p "Shutdown Mode (immediate/delayed/log-only) [log-only]: " SMODE
    SMODE=${SMODE:-log-only}

    read -p "Enable Dry Run (true/false) [true]: " DRUN
    DRUN=${DRUN:-true}

    if [[ "$DRUN" == "true" ]]; then
        print_warn "Dry Run is ENABLED. It will NOT actually shut down your server."
    elif [[ "$SMODE" != "log-only" ]]; then
        print_warn "Dry Run is DISABLED. OpenUPS WILL shut down your server if $TARGET fails."
    fi

    # Write environment file
    echo "# OpenUPS Environment File
OPENUPS_TARGET=$TARGET
OPENUPS_INTERVAL=$INTERVAL
OPENUPS_THRESHOLD=$THRESHOLD
OPENUPS_TIMEOUT=2000
OPENUPS_PAYLOAD_SIZE=56
OPENUPS_RETRIES=2
OPENUPS_DRY_RUN=$DRUN
OPENUPS_SHUTDOWN_MODE=$SMODE
OPENUPS_LOG_LEVEL=info
OPENUPS_TIMESTAMP=false
OPENUPS_STATE_FILE=/run/openups.state.json
OPENUPS_SYSTEMD=true
OPENUPS_WATCHDOG=false
" | tee $ENV_FILE > /dev/null

    chmod 644 $ENV_FILE
    print_step "Configuration saved to $ENV_FILE"
}

install_service() {
    print_step "Installing binary to $BIN_PATH..."
    cp "${PROJECT_DIR}/bin/openups" "$BIN_PATH"
    chmod 755 "$BIN_PATH"
    print_step "Assigning CAP_NET_RAW capability..."
    setcap cap_net_raw+ep "$BIN_PATH" || print_warn "Failed to setcap. You might lack 'setcap' tool (install libcap2-bin)."

    print_step "Installing systemd service..."
    cp "${PROJECT_DIR}/systemd/${SERVICE_NAME}" "$SERVICE_PATH"
    systemctl daemon-reload
    systemctl enable "$SERVICE_NAME"
    
    # Check if user wants to start it
    read -p "Do you want to start the service now? (y/n) [y]: " START_NOW
    START_NOW=${START_NOW:-y}
    if [[ "$START_NOW" =~ ^[Yy]$ ]]; then
        systemctl start "$SERVICE_NAME"
        echo 
        systemctl status "$SERVICE_NAME" --no-pager
    fi
    print_step "Installation complete!"
}

update_project() {
    build_project
    install_service
}

uninstall_project() {
    print_step "Uninstalling OpenUPS..."
    systemctl disable --now "$SERVICE_NAME" || true
    rm -f "$BIN_PATH" "$SERVICE_PATH" "$ENV_FILE"
    systemctl daemon-reload
    print_step "Uninstallation complete."
}

# Main command mapping
CMD=$1
case $CMD in
    install)
        check_root
        build_project
        configure_env
        install_service
        ;;
    update)
        check_root
        update_project
        ;;
    uninstall)
        check_root
        uninstall_project
        ;;
    *)
        echo "Usage: sudo ./install.sh [install|update|uninstall]"
        exit 1
        ;;
esac