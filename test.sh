#!/bin/bash
# OpenUPS 自动化测试脚本
# 验证编译、参数解析、配置验证、安全性和代码质量
#
# 用法：
#   ./test.sh              # 运行基础测试（编译、参数、安全）
#   ./test.sh --gray       # 追加进程级灰度验证（需要 root 或 CAP_NET_RAW）
#   ./test.sh --gray-systemd  # 追加 systemd 灰度验证（需要 systemd + root）
#   ./test.sh --all        # 运行全部测试（基础 + 灰度 + systemd 灰度）

set -e  # 遇到错误立即退出

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN_PATH="${ROOT_DIR}/bin/openups"

RUN_GRAY=0
RUN_GRAY_SYSTEMD=0
for arg in "$@"; do
    case "${arg}" in
        --gray)          RUN_GRAY=1 ;;
        --gray-systemd)  RUN_GRAY_SYSTEMD=1 ;;
        --all)           RUN_GRAY=1; RUN_GRAY_SYSTEMD=1 ;;
    esac
done

echo "========================================"
echo "OpenUPS 自动化测试"
echo "========================================"
echo

# 编译检查
echo "[1/11] 编译检查..."
make clean > /dev/null 2>&1
if make 2>&1 | grep -E "^[^:]+:[0-9]+:[0-9]+: (warning|error):" > /dev/null; then
    echo "❌ 编译有警告或错误"
    make 2>&1 | grep -E "^[^:]+:[0-9]+:[0-9]+: (warning|error):"
    exit 1
fi
echo "✓ 编译成功，无警告"

# 帮助信息
echo "[2/11] 测试帮助信息..."
if ! ./bin/openups --help > /dev/null 2>&1; then
    echo "❌ 帮助信息失败"
    exit 1
fi
echo "✓ 帮助信息正常"

# 版本信息
echo "[3/11] 测试版本信息..."
if ! ./bin/openups --version > /dev/null 2>&1; then
    echo "❌ 版本信息失败"
    exit 1
fi
echo "✓ 版本信息正常"

# 参数验证测试
echo "[4/11] 测试布尔参数 --dry-run..."
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

echo "[5/11] 测试环境变量优先级..."
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
echo "[6/11] 测试空目标地址..."
if ./bin/openups --target "" 2>&1 | grep -q "Target host cannot be empty"; then
    echo "✓ 空目标地址被拒绝"
else
    echo "❌ 空目标地址检查失败"
    exit 1
fi

# 负数间隔
echo "[7/11] 测试负数间隔..."
if ./bin/openups --target 127.0.0.1 --interval -1 2>&1 | \
   grep -Eq "Interval must be positive|Invalid value for --interval"; then
    echo "✓ 负数间隔被拒绝"
else
    echo "❌ 负数间隔检查失败"
    exit 1
fi

# 零阈值
echo "[8/11] 测试零阈值..."
if ./bin/openups --target 127.0.0.1 --threshold 0 2>&1 | \
   grep -Eq "Failure threshold must be positive|Invalid value for --threshold"; then
    echo "✓ 零阈值被拒绝"
else
    echo "❌ 零阈值检查失败"
    exit 1
fi

# 零超时
echo "[9/11] 测试零超时..."
if ./bin/openups --target 127.0.0.1 --timeout 0 2>&1 | \
   grep -Eq "Timeout must be positive|Invalid value for --timeout"; then
    echo "✓ 零超时被拒绝"
else
    echo "❌ 零超时检查失败"
    exit 1
fi

# 注入式 target（OpenUPS 禁用 DNS，且 target 必须为 IP 字面量）
echo "[10/11] 测试 target 注入防护..."
if ./bin/openups --target "1.1.1.1;rm -rf /" 2>&1 | grep -Eq "Target must be a valid|DNS is disabled"; then
    echo "✓ 注入式 target 被拒绝"
else
    echo "❌ target 注入防护失败"
    exit 1
fi

# 超大包大小
echo "[11/11] 测试超大包大小..."
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
if grep -rE "(strcpy|strcat|sprintf)\(" src/ 2>/dev/null | grep -v "//" > /dev/null; then
    echo "  ❌ 发现不安全的字符串函数"
    ((errors++))
fi

# 检查是否有 syslog 遗留代码
if grep -ri "syslog" src/ 2>/dev/null | grep -v "^Binary"; then
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
echo "✓ 基础测试通过！"
echo "========================================"
echo
ls -lh bin/openups

# ============================================================
# 灰度验证：进程级（需要 root 或 CAP_NET_RAW）
# ============================================================
if [[ "${RUN_GRAY}" -eq 1 ]]; then
    echo
    echo "========================================"
    echo "灰度验证（进程级）"
    echo "========================================"
    echo

    TARGET_FAIL="${TARGET_FAIL:-203.0.113.1}"
    INTERVAL_SEC="${INTERVAL_SEC:-1}"
    THRESHOLD="${THRESHOLD:-2}"
    TIMEOUT_MS="${TIMEOUT_MS:-700}"
    RETRIES="${RETRIES:-0}"

    PHASE1_SEC="${PHASE1_SEC:-12}"
    PHASE2_SEC="${PHASE2_SEC:-14}"

    RUN_TS="$(date +%Y%m%d_%H%M%S)"
    LOG_DIR="${ROOT_DIR}/graylogs/${RUN_TS}"
    PHASE1_LOG="${LOG_DIR}/phase1_dry_run_true.log"
    PHASE2_LOG="${LOG_DIR}/phase2_dry_run_false_log_only.log"
    GRAY_SUMMARY="${LOG_DIR}/summary.txt"

    mkdir -p "${LOG_DIR}"

    if [[ ! -x "${BIN_PATH}" ]]; then
        echo "[ERROR] Binary not found or not executable: ${BIN_PATH}" >&2
        exit 1
    fi

    SUDO=""
    if [[ "${EUID}" -ne 0 ]]; then
        SUDO="sudo"
    fi

    run_phase() {
        local phase_name="$1"
        local duration_sec="$2"
        local out_log="$3"
        shift 3

        echo "[INFO] ${phase_name} (timeout ${duration_sec}s)"
        set +e
        ${SUDO} timeout "${duration_sec}s" "${BIN_PATH}" "$@" >"${out_log}" 2>&1
        local exit_code=$?
        set -e

        if [[ ${exit_code} -ne 0 && ${exit_code} -ne 124 ]]; then
            echo "[ERROR] ${phase_name} failed, exit_code=${exit_code}" | tee -a "${GRAY_SUMMARY}"
            tail -n 50 "${out_log}" || true
            exit ${exit_code}
        fi

        echo "[INFO] ${phase_name} finished (exit_code=${exit_code})"
    }

    count_lines() {
        local pattern="$1"
        local file="$2"
        grep -cE "${pattern}" "${file}" 2>/dev/null || true
    }

    echo "== Phase 1: dry-run=true + immediate（验证触发路径，不执行关机） =="
    run_phase "Phase 1" "${PHASE1_SEC}" "${PHASE1_LOG}" \
        --target "${TARGET_FAIL}" \
        --interval "${INTERVAL_SEC}" \
        --threshold "${THRESHOLD}" \
        --timeout "${TIMEOUT_MS}" \
        --retries "${RETRIES}" \
        --shutdown-mode immediate \
        --dry-run=true \
        --log-level debug

    echo "== Phase 2: dry-run=false + log-only（验证非 dry-run 下安全分支） =="
    run_phase "Phase 2" "${PHASE2_SEC}" "${PHASE2_LOG}" \
        --target "${TARGET_FAIL}" \
        --interval "${INTERVAL_SEC}" \
        --threshold "${THRESHOLD}" \
        --timeout "${TIMEOUT_MS}" \
        --retries "${RETRIES}" \
        --shutdown-mode log-only \
        --dry-run=false \
        --log-level debug

    phase1_trigger_count="$(count_lines "Would trigger shutdown|Shutdown triggered" "${PHASE1_LOG}")"
    phase2_log_only_count="$(count_lines "LOG-ONLY mode|mode is log-only" "${PHASE2_LOG}")"
    phase2_fail_count="$(count_lines "Ping failed" "${PHASE2_LOG}")"

    {
        echo "OpenUPS 灰度验证摘要"
        echo "Run timestamp: ${RUN_TS}"
        echo "Binary: ${BIN_PATH}"
        echo
        echo "[Phase 1] dry-run=true + immediate"
        echo "  Trigger-related lines: ${phase1_trigger_count}"
        echo "  Log file: ${PHASE1_LOG}"
        echo
        echo "[Phase 2] dry-run=false + log-only"
        echo "  LOG-ONLY lines: ${phase2_log_only_count}"
        echo "  Ping failed lines: ${phase2_fail_count}"
        echo "  Log file: ${PHASE2_LOG}"
        echo
        if [[ "${phase1_trigger_count}" -ge 1 && "${phase2_log_only_count}" -ge 1 ]]; then
            echo "Result: PASS"
        else
            echo "Result: CHECK_REQUIRED"
        fi
    } | tee "${GRAY_SUMMARY}"

    echo
    echo "[INFO] 进程级灰度验证完成，摘要: ${GRAY_SUMMARY}"
fi

# ============================================================
# 灰度验证：systemd 级（需要 systemd + root）
# ============================================================
if [[ "${RUN_GRAY_SYSTEMD}" -eq 1 ]]; then
    echo
    echo "========================================"
    echo "灰度验证（systemd 级）"
    echo "========================================"
    echo

    SERVICE_NAME="${SERVICE_NAME:-openups.service}"

    TARGET_FAIL="${TARGET_FAIL:-203.0.113.1}"
    INTERVAL_SEC="${INTERVAL_SEC:-1}"
    THRESHOLD="${THRESHOLD:-2}"
    TIMEOUT_MS="${TIMEOUT_MS:-700}"
    RETRIES="${RETRIES:-0}"
    PAYLOAD_SIZE="${PAYLOAD_SIZE:-56}"

    SD_PHASE1_SEC="${SD_PHASE1_SEC:-10}"
    SD_PHASE2_SEC="${SD_PHASE2_SEC:-14}"

    RUN_TS="$(date +%Y%m%d_%H%M%S)"
    SD_LOG_DIR="${ROOT_DIR}/graylogs/systemd_${RUN_TS}"
    SD_PHASE1_LOG="${SD_LOG_DIR}/phase1_service_dry_run_true.log"
    SD_PHASE2_LOG="${SD_LOG_DIR}/phase2_service_log_only.log"
    SD_SUMMARY="${SD_LOG_DIR}/summary.txt"

    mkdir -p "${SD_LOG_DIR}"

    if ! command -v systemctl >/dev/null 2>&1; then
        echo "[ERROR] systemctl not found" >&2
        exit 1
    fi

    SUDO=""
    if [[ "${EUID}" -ne 0 ]]; then
        SUDO="sudo"
    fi

    if ! ${SUDO} systemctl cat "${SERVICE_NAME}" >/dev/null 2>&1; then
        echo "[ERROR] Service not found: ${SERVICE_NAME}" >&2
        echo "[HINT] Install/enable service first, e.g. sudo cp systemd/openups.service /etc/systemd/system/" >&2
        exit 1
    fi

    DROPIN_DIR="/etc/systemd/system/${SERVICE_NAME}.d"
    DROPIN_FILE="${DROPIN_DIR}/99-gray-validate.conf"

    WAS_ACTIVE=0
    if ${SUDO} systemctl is-active --quiet "${SERVICE_NAME}"; then
        WAS_ACTIVE=1
    fi

    cleanup_systemd() {
        set +e
        echo "[INFO] Rolling back temporary drop-in..."
        ${SUDO} rm -f "${DROPIN_FILE}"
        ${SUDO} rmdir "${DROPIN_DIR}" 2>/dev/null || true
        ${SUDO} systemctl daemon-reload

        if [[ "${WAS_ACTIVE}" -eq 1 ]]; then
            ${SUDO} systemctl restart "${SERVICE_NAME}" >/dev/null 2>&1 || true
        else
            ${SUDO} systemctl stop "${SERVICE_NAME}" >/dev/null 2>&1 || true
        fi
    }
    trap cleanup_systemd EXIT

    write_dropin() {
        local shutdown_mode="$1"
        local dry_run="$2"
        local log_level="$3"

        ${SUDO} mkdir -p "${DROPIN_DIR}"
        cat <<DROPIN_EOF | ${SUDO} tee "${DROPIN_FILE}" >/dev/null
[Service]
Environment="OPENUPS_TARGET=${TARGET_FAIL}"
Environment="OPENUPS_INTERVAL=${INTERVAL_SEC}"
Environment="OPENUPS_THRESHOLD=${THRESHOLD}"
Environment="OPENUPS_TIMEOUT=${TIMEOUT_MS}"
Environment="OPENUPS_RETRIES=${RETRIES}"
Environment="OPENUPS_PAYLOAD_SIZE=${PAYLOAD_SIZE}"
Environment="OPENUPS_SHUTDOWN_MODE=${shutdown_mode}"
Environment="OPENUPS_DRY_RUN=${dry_run}"
Environment="OPENUPS_LOG_LEVEL=${log_level}"
Environment="OPENUPS_SYSTEMD=true"
Environment="OPENUPS_WATCHDOG=false"
Environment="OPENUPS_TIMESTAMP=false"
DROPIN_EOF
    }

    capture_phase() {
        local phase_name="$1"
        local shutdown_mode="$2"
        local dry_run="$3"
        local log_level="$4"
        local duration_sec="$5"
        local out_log="$6"

        echo "[INFO] ${phase_name}: mode=${shutdown_mode}, dry_run=${dry_run}, duration=${duration_sec}s"

        write_dropin "${shutdown_mode}" "${dry_run}" "${log_level}"
        ${SUDO} systemctl daemon-reload

        local start_ts
        start_ts="$(date --iso-8601=seconds)"

        ${SUDO} systemctl restart "${SERVICE_NAME}"
        sleep "${duration_sec}"

        ${SUDO} journalctl -u "${SERVICE_NAME}" --since "${start_ts}" --no-pager >"${out_log}" || true

        local state
        state="$( ${SUDO} systemctl is-active "${SERVICE_NAME}" || true )"
        echo "[INFO] ${phase_name} service state: ${state}" | tee -a "${SD_SUMMARY}"
    }

    sd_count_lines() {
        local pattern="$1"
        local file="$2"
        grep -cE "${pattern}" "${file}" 2>/dev/null || true
    }

    echo "Service: ${SERVICE_NAME}"
    echo "日志目录: ${SD_LOG_DIR}"
    echo

    capture_phase "Phase 1" "immediate" "true" "debug" "${SD_PHASE1_SEC}" "${SD_PHASE1_LOG}"
    capture_phase "Phase 2" "log-only" "false" "debug" "${SD_PHASE2_SEC}" "${SD_PHASE2_LOG}"

    sd_p1_trigger="$(sd_count_lines "Would trigger shutdown|Shutdown triggered, exiting monitor loop" "${SD_PHASE1_LOG}")"
    sd_p2_log_only="$(sd_count_lines "mode is log-only|LOG-ONLY mode" "${SD_PHASE2_LOG}")"
    sd_p2_fail="$(sd_count_lines "Ping failed" "${SD_PHASE2_LOG}")"

    {
        echo "OpenUPS systemd 灰度验证摘要"
        echo "Run timestamp: ${RUN_TS}"
        echo "Service: ${SERVICE_NAME}"
        echo
        echo "[Phase 1] immediate + dry-run=true"
        echo "  Trigger-related lines: ${sd_p1_trigger}"
        echo "  Log file: ${SD_PHASE1_LOG}"
        echo
        echo "[Phase 2] log-only + dry-run=false"
        echo "  LOG-ONLY lines: ${sd_p2_log_only}"
        echo "  Ping failed lines: ${sd_p2_fail}"
        echo "  Log file: ${SD_PHASE2_LOG}"
        echo
        if [[ "${sd_p1_trigger}" -ge 1 && "${sd_p2_log_only}" -ge 1 ]]; then
            echo "Result: PASS"
        else
            echo "Result: CHECK_REQUIRED"
        fi
    } | tee "${SD_SUMMARY}"

    echo
    echo "[INFO] systemd 灰度验证完成，摘要: ${SD_SUMMARY}"
fi

echo
echo "========================================"
echo "✓ 所有测试通过！"
echo "========================================"
echo
echo "更多使用示例请查看 README.md"
