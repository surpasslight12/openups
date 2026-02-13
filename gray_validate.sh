#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN_PATH="${ROOT_DIR}/bin/openups"

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
SUMMARY_LOG="${LOG_DIR}/summary.txt"

mkdir -p "${LOG_DIR}"

if [[ ! -x "${BIN_PATH}" ]]; then
    echo "[ERROR] Binary not found or not executable: ${BIN_PATH}" >&2
    echo "[HINT] Run: make" >&2
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
        echo "[ERROR] ${phase_name} failed, exit_code=${exit_code}" | tee -a "${SUMMARY_LOG}"
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

echo "OpenUPS 灰度脚本化验证"
echo "日志目录: ${LOG_DIR}"
echo

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
} | tee "${SUMMARY_LOG}"

echo
echo "[INFO] 验证完成，摘要: ${SUMMARY_LOG}"