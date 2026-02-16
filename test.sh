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

# 测试计数器
TESTS_PASSED=0
TESTS_TOTAL=0

# 通用测试函数：run_test <测试名> <命令...>
# 成功时打印 ✓，失败时打印 ❌ 并退出
run_test() {
    local name="$1"
    shift
    TESTS_TOTAL=$((TESTS_TOTAL + 1))
    echo "[${TESTS_TOTAL}] ${name}..."
    if "$@" > /dev/null 2>&1; then
        echo "  ✓ ${name}"
        TESTS_PASSED=$((TESTS_PASSED + 1))
    else
        echo "  ❌ ${name}"
        exit 1
    fi
}

# 验证命令输出包含预期模式
# expect_output_match <描述> <模式(ERE)> <命令...>
expect_output_match() {
    local name="$1"
    local pattern="$2"
    shift 2
    TESTS_TOTAL=$((TESTS_TOTAL + 1))
    echo "[${TESTS_TOTAL}] ${name}..."
    if "$@" 2>&1 | grep -Eq "${pattern}"; then
        echo "  ✓ ${name}"
        TESTS_PASSED=$((TESTS_PASSED + 1))
    else
        echo "  ❌ ${name}"
        exit 1
    fi
}

echo "========================================"
echo "OpenUPS 自动化测试"
echo "========================================"
echo

# ---- 编译检查 ----
echo "--- 编译 ---"
TESTS_TOTAL=$((TESTS_TOTAL + 1))
echo "[${TESTS_TOTAL}] 编译检查（无警告无错误）..."
make clean > /dev/null 2>&1
if make 2>&1 | grep -E "^[^:]+:[0-9]+:[0-9]+: (warning|error):" > /dev/null; then
    echo "  ❌ 编译有警告或错误"
    make 2>&1 | grep -E "^[^:]+:[0-9]+:[0-9]+: (warning|error):"
    exit 1
fi
echo "  ✓ 编译成功，无警告"
TESTS_PASSED=$((TESTS_PASSED + 1))

# ---- 基本功能 ----
echo ""
echo "--- 基本功能 ---"
run_test "帮助信息 (--help)" ./bin/openups --help
run_test "版本信息 (--version)" ./bin/openups --version

# ---- 参数解析 ----
echo ""
echo "--- 参数解析 ---"
run_test "布尔参数 --dry-run=true" ./bin/openups --help --dry-run=true
run_test "布尔参数 --dry-run=false" ./bin/openups --help --dry-run=false
run_test "短选项 -dfalse" ./bin/openups --help -dfalse
run_test "环境变量配置" env OPENUPS_TARGET=127.0.0.1 OPENUPS_INTERVAL=5 OPENUPS_THRESHOLD=3 ./bin/openups --help
run_test "CLI 优先级高于环境变量" env OPENUPS_TARGET=127.0.0.1 ./bin/openups --target 8.8.8.8 --help

# ---- 输入验证 ----
echo ""
echo "--- 输入验证 ---"
expect_output_match "空目标地址被拒绝" \
    "Target host cannot be empty" \
    ./bin/openups --target ""

expect_output_match "负数间隔被拒绝" \
    "Interval must be positive|Invalid value for --interval" \
    ./bin/openups --target 127.0.0.1 --interval -1

expect_output_match "零阈值被拒绝" \
    "Failure threshold must be positive|Invalid value for --threshold" \
    ./bin/openups --target 127.0.0.1 --threshold 0

expect_output_match "零超时被拒绝" \
    "Timeout must be positive|Invalid value for --timeout" \
    ./bin/openups --target 127.0.0.1 --timeout 0

expect_output_match "注入式 target 被拒绝" \
    "Target must be a valid|DNS is disabled" \
    ./bin/openups --target "1.1.1.1;rm -rf /"

expect_output_match "超大包大小被拒绝" \
    "Payload size must be between|Invalid value for --payload-size" \
    ./bin/openups --target 127.0.0.1 --payload-size 99999

# ---- 代码质量检查 ----
echo ""
echo "--- 代码质量 ---"
quality_errors=0

# 检查不安全的字符串函数
if grep -rnE '\b(strcpy|strcat|sprintf)\s*\(' src/ 2>/dev/null > /dev/null; then
    echo "  ❌ 发现不安全的字符串函数（strcpy/strcat/sprintf）"
    quality_errors=$((quality_errors + 1))
fi

# 检查 syslog 遗留代码
if grep -rn 'syslog' src/ 2>/dev/null > /dev/null; then
    echo "  ⚠️ 发现 syslog 遗留代码"
    quality_errors=$((quality_errors + 1))
fi

if [ $quality_errors -eq 0 ]; then
    echo "  ✓ 代码质量良好（无不安全函数，无 syslog 遗留）"
fi

# ---- 二进制体积 ----
echo ""
echo "--- 二进制体积 ---"
binary_size=$(stat -c %s bin/openups 2>/dev/null || stat -f %z bin/openups 2>/dev/null)
if [ -n "$binary_size" ]; then
    size_kb=$(echo "$binary_size" | awk '{printf "%.1f", $1/1024}')
    if [ "$binary_size" -lt 524288 ]; then  # < 512 KB
        echo "  ✓ 二进制体积: ${size_kb} KB"
    else
        echo "  ⚠️ 二进制体积偏大: ${size_kb} KB"
    fi
fi

# ---- 安全加固 ----
echo ""
echo "--- 安全加固 ---"
sec_errors=0
if command -v readelf > /dev/null 2>&1; then
    # RELRO
    if readelf -l bin/openups 2>/dev/null | grep -q "GNU_RELRO"; then
        echo "  ✓ RELRO: enabled"
    else
        echo "  ❌ RELRO: not found"
        sec_errors=$((sec_errors + 1))
    fi
    # PIE
    if readelf -h bin/openups 2>/dev/null | grep -q "DYN"; then
        echo "  ✓ PIE: enabled"
    else
        echo "  ❌ PIE: not found"
        sec_errors=$((sec_errors + 1))
    fi
    # Stack Canary
    if readelf -s bin/openups 2>/dev/null | grep -q "__stack_chk_fail"; then
        echo "  ✓ Stack Canary: enabled"
    else
        echo "  ❌ Stack Canary: not found"
        sec_errors=$((sec_errors + 1))
    fi
    # NX Stack
    if readelf -l bin/openups 2>/dev/null | grep "GNU_STACK" | grep -qv "RWE"; then
        echo "  ✓ NX Stack: enabled"
    else
        echo "  ❌ NX Stack: not found"
        sec_errors=$((sec_errors + 1))
    fi
    # FORTIFY_SOURCE
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
echo "✓ 基础测试通过！(${TESTS_PASSED}/${TESTS_TOTAL})"
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
    PROC_SUMMARY="${LOG_DIR}/summary.txt"

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
            echo "[ERROR] ${phase_name} failed, exit_code=${exit_code}" | tee -a "${PROC_SUMMARY}"
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

    # ---- 连续运行测试 ----
    echo ""
    echo "== Phase 3: 连续运行测试（信号处理 + 优雅关闭） =="

    PHASE3_LOG="${LOG_DIR}/phase3_continuous_run.log"
    PHASE4_LOG="${LOG_DIR}/phase4_sigusr1_stats.log"
    PHASE5_LOG="${LOG_DIR}/phase5_log_only_continuous.log"

    # Phase 3: SIGTERM 优雅关闭测试
    echo "[INFO] Phase 3: 连续运行 + SIGTERM 优雅关闭"
    set +e
    ${SUDO} "${BIN_PATH}" \
        --target 127.0.0.1 \
        --interval 1 \
        --threshold 10 \
        --dry-run=true \
        --log-level debug >"${PHASE3_LOG}" 2>&1 &
    PHASE3_PID=$!
    sleep 3
    ${SUDO} kill -15 "${PHASE3_PID}" 2>/dev/null || true
    wait "${PHASE3_PID}" 2>/dev/null
    PHASE3_EXIT=$?
    set -e

    phase3_ping_count="$(count_lines "Ping successful" "${PHASE3_LOG}")"
    phase3_graceful="$(count_lines "stopping gracefully|monitor stopped" "${PHASE3_LOG}")"

    TESTS_TOTAL=$((TESTS_TOTAL + 1))
    if [[ "${phase3_ping_count}" -ge 2 && "${phase3_graceful}" -ge 1 ]]; then
        echo "  ✓ Phase 3: 连续运行 + SIGTERM 优雅关闭 (${phase3_ping_count} pings, graceful shutdown)"
        TESTS_PASSED=$((TESTS_PASSED + 1))
    else
        echo "  ❌ Phase 3: 连续运行测试失败 (pings=${phase3_ping_count}, graceful=${phase3_graceful})"
        tail -n 20 "${PHASE3_LOG}" || true
        exit 1
    fi

    # Phase 4: SIGUSR1 统计信息测试
    echo "[INFO] Phase 4: SIGUSR1 统计信息输出"
    set +e
    ${SUDO} "${BIN_PATH}" \
        --target 127.0.0.1 \
        --interval 1 \
        --threshold 10 \
        --dry-run=true \
        --log-level debug >"${PHASE4_LOG}" 2>&1 &
    PHASE4_PID=$!
    sleep 3
    ${SUDO} kill -10 "${PHASE4_PID}" 2>/dev/null || true
    sleep 1
    ${SUDO} kill -15 "${PHASE4_PID}" 2>/dev/null || true
    wait "${PHASE4_PID}" 2>/dev/null
    set -e

    phase4_stats="$(count_lines "Statistics:" "${PHASE4_LOG}")"

    TESTS_TOTAL=$((TESTS_TOTAL + 1))
    if [[ "${phase4_stats}" -ge 2 ]]; then
        echo "  ✓ Phase 4: SIGUSR1 统计信息输出 (${phase4_stats} statistics lines)"
        TESTS_PASSED=$((TESTS_PASSED + 1))
    else
        echo "  ❌ Phase 4: SIGUSR1 统计信息测试失败 (stats=${phase4_stats})"
        tail -n 20 "${PHASE4_LOG}" || true
        exit 1
    fi

    # Phase 5: log-only 模式连续运行（不退出）
    echo "[INFO] Phase 5: log-only 模式连续运行验证"
    PHASE5_DURATION=8
    set +e
    ${SUDO} timeout "${PHASE5_DURATION}s" "${BIN_PATH}" \
        --target "${TARGET_FAIL}" \
        --interval 1 \
        --threshold 2 \
        --timeout "${TIMEOUT_MS}" \
        --retries "${RETRIES}" \
        --shutdown-mode log-only \
        --dry-run=false \
        --log-level debug >"${PHASE5_LOG}" 2>&1
    PHASE5_EXIT=$?
    set -e

    phase5_log_only="$(count_lines "LOG-ONLY mode" "${PHASE5_LOG}")"

    TESTS_TOTAL=$((TESTS_TOTAL + 1))
    # exit code 124 = timeout killed it (expected: program kept running)
    if [[ "${PHASE5_EXIT}" -eq 124 && "${phase5_log_only}" -ge 2 ]]; then
        echo "  ✓ Phase 5: log-only 模式连续运行 (${phase5_log_only} log-only triggers, kept running)"
        TESTS_PASSED=$((TESTS_PASSED + 1))
    else
        echo "  ❌ Phase 5: log-only 连续运行测试失败 (exit=${PHASE5_EXIT}, log_only=${phase5_log_only})"
        tail -n 20 "${PHASE5_LOG}" || true
        exit 1
    fi

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
        echo "[Phase 3] 连续运行 + SIGTERM"
        echo "  Ping count: ${phase3_ping_count}"
        echo "  Graceful shutdown: ${phase3_graceful}"
        echo "  Log file: ${PHASE3_LOG}"
        echo
        echo "[Phase 4] SIGUSR1 统计信息"
        echo "  Statistics lines: ${phase4_stats}"
        echo "  Log file: ${PHASE4_LOG}"
        echo
        echo "[Phase 5] log-only 连续运行"
        echo "  LOG-ONLY triggers: ${phase5_log_only}"
        echo "  Exit code: ${PHASE5_EXIT} (expected: 124)"
        echo "  Log file: ${PHASE5_LOG}"
        echo
        if [[ "${phase1_trigger_count}" -ge 1 && "${phase2_log_only_count}" -ge 1 && \
              "${phase3_ping_count}" -ge 2 && "${phase4_stats}" -ge 2 && \
              "${phase5_log_only}" -ge 2 ]]; then
            echo "Result: PASS"
        else
            echo "Result: CHECK_REQUIRED"
        fi
    } | tee "${PROC_SUMMARY}"

    echo
    echo "[INFO] 进程级灰度验证完成，摘要: ${PROC_SUMMARY}"
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
