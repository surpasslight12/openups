#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVICE_NAME="${SERVICE_NAME:-openups.service}"

TARGET_FAIL="${TARGET_FAIL:-203.0.113.1}"
INTERVAL_SEC="${INTERVAL_SEC:-1}"
THRESHOLD="${THRESHOLD:-2}"
TIMEOUT_MS="${TIMEOUT_MS:-700}"
RETRIES="${RETRIES:-0}"
PAYLOAD_SIZE="${PAYLOAD_SIZE:-56}"

PHASE1_SEC="${PHASE1_SEC:-10}"
PHASE2_SEC="${PHASE2_SEC:-14}"

RUN_TS="$(date +%Y%m%d_%H%M%S)"
LOG_DIR="${ROOT_DIR}/graylogs/systemd_${RUN_TS}"
PHASE1_LOG="${LOG_DIR}/phase1_service_dry_run_true.log"
PHASE2_LOG="${LOG_DIR}/phase2_service_log_only.log"
SUMMARY_LOG="${LOG_DIR}/summary.txt"

mkdir -p "${LOG_DIR}"

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

cleanup() {
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
trap cleanup EXIT

write_dropin() {
    local shutdown_mode="$1"
    local dry_run="$2"
    local log_level="$3"

    ${SUDO} mkdir -p "${DROPIN_DIR}"
    cat <<EOF | ${SUDO} tee "${DROPIN_FILE}" >/dev/null
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
EOF
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
    echo "[INFO] ${phase_name} service state: ${state}" | tee -a "${SUMMARY_LOG}"
}

count_lines() {
    local pattern="$1"
    local file="$2"
    grep -cE "${pattern}" "${file}" 2>/dev/null || true
}

echo "OpenUPS systemd 灰度脚本化验证"
echo "Service: ${SERVICE_NAME}"
echo "日志目录: ${LOG_DIR}"
echo

capture_phase "Phase 1" "immediate" "true" "debug" "${PHASE1_SEC}" "${PHASE1_LOG}"
capture_phase "Phase 2" "log-only" "false" "debug" "${PHASE2_SEC}" "${PHASE2_LOG}"

phase1_trigger_count="$(count_lines "Would trigger shutdown|Shutdown triggered, exiting monitor loop" "${PHASE1_LOG}")"
phase2_log_only_count="$(count_lines "mode is log-only|LOG-ONLY mode" "${PHASE2_LOG}")"
phase2_fail_count="$(count_lines "Ping failed" "${PHASE2_LOG}")"

{
    echo "OpenUPS systemd 灰度验证摘要"
    echo "Run timestamp: ${RUN_TS}"
    echo "Service: ${SERVICE_NAME}"
    echo
    echo "[Phase 1] immediate + dry-run=true"
    echo "  Trigger-related lines: ${phase1_trigger_count}"
    echo "  Log file: ${PHASE1_LOG}"
    echo
    echo "[Phase 2] log-only + dry-run=false"
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
echo "[INFO] 完成。摘要: ${SUMMARY_LOG}"
