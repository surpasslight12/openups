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
set -o pipefail  # 让管道中的编译失败和 grep 失败都能正确传播

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
    local output
    TESTS_TOTAL=$((TESTS_TOTAL + 1))
    echo "[${TESTS_TOTAL}] ${name}..."
    output="$("$@" 2>&1 || true)"
    if grep -Eq "${pattern}" <<< "${output}"; then
        echo "  ✓ ${name}"
        TESTS_PASSED=$((TESTS_PASSED + 1))
    else
        echo "  ❌ ${name}"
        if [ -n "${output}" ]; then
            echo "${output}"
        fi
        exit 1
    fi
}

count_lines() {
    local pattern="$1"
    local file="$2"
    grep -cE "${pattern}" "${file}" 2>/dev/null || true
}

assert_count_at_least() {
    local name="$1"
    local actual="$2"
    local expected_min="$3"
    local success_message="$4"
    local failure_message="$5"
    local log_file="$6"

    TESTS_TOTAL=$((TESTS_TOTAL + 1))
    if [[ "${actual}" -ge "${expected_min}" ]]; then
        echo "  ✓ ${name}: ${success_message}"
        TESTS_PASSED=$((TESTS_PASSED + 1))
    else
        echo "  ❌ ${name}: ${failure_message}"
        tail -n 20 "${log_file}" || true
        exit 1
    fi
}

assert_exit_and_count() {
    local name="$1"
    local exit_code="$2"
    local expected_exit="$3"
    local actual_count="$4"
    local expected_min="$5"
    local success_message="$6"
    local failure_message="$7"
    local log_file="$8"

    TESTS_TOTAL=$((TESTS_TOTAL + 1))
    if [[ "${exit_code}" -eq "${expected_exit}" && "${actual_count}" -ge "${expected_min}" ]]; then
        echo "  ✓ ${name}: ${success_message}"
        TESTS_PASSED=$((TESTS_PASSED + 1))
    else
        echo "  ❌ ${name}: ${failure_message}"
        tail -n 20 "${log_file}" || true
        exit 1
    fi
}

run_internal_c_test() {
    local name="$1"
    local source_path="$2"
    local binary_path="$3"
    local build_log="$4"
    shift 4

    TESTS_TOTAL=$((TESTS_TOTAL + 1))
    echo "[${TESTS_TOTAL}] ${name}..."

    if ! "${CC:-gcc}" \
        -std=c23 -Wall -Wextra -Werror \
        -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
        -I"${ROOT_DIR}" -I"${ROOT_DIR}/src" \
        "${source_path}" "$@" -o "${binary_path}" >"${build_log}" 2>&1; then
        echo "  ❌ ${name}（编译失败）"
        cat "${build_log}"
        exit 1
    fi

    if ! "${binary_path}" >"${build_log}" 2>&1; then
        echo "  ❌ ${name}（运行失败）"
        cat "${build_log}"
        exit 1
    fi

    echo "  ✓ ${name}"
    TESTS_PASSED=$((TESTS_PASSED + 1))
}

write_monitor_error_harness() {
        local source_path="$1"
        local scenario="$2"

        local send_stub='bool icmp_pinger_send_echo(icmp_pinger_t *restrict pinger,
        const struct sockaddr_storage *restrict dest_addr,
        socklen_t dest_addr_len, uint16_t identifier, size_t packet_len,
        char *restrict error_msg, size_t error_size) {
    (void)pinger;
    (void)dest_addr;
    (void)dest_addr_len;
    (void)identifier;
    (void)packet_len;
    (void)error_msg;
    (void)error_size;
    return true;
}'
        local receive_stub='icmp_receive_status_t icmp_pinger_receive_reply(
        const icmp_pinger_t *restrict pinger,
        const struct sockaddr_storage *restrict dest_addr, uint16_t identifier,
        uint16_t expected_sequence, uint64_t send_time_ms, uint64_t now_ms,
        ping_result_t *restrict out_result) {
    (void)pinger;
    (void)dest_addr;
    (void)identifier;
    (void)expected_sequence;
    (void)send_time_ms;
    (void)now_ms;
    (void)out_result;
    return ICMP_RECEIVE_NO_MORE;
}'
        local exercise_body=''
        local state_assertions=''
        local expected_log=''

        case "${scenario}" in
                receive)
                        receive_stub='icmp_receive_status_t icmp_pinger_receive_reply(
        const icmp_pinger_t *restrict pinger,
        const struct sockaddr_storage *restrict dest_addr, uint16_t identifier,
        uint16_t expected_sequence, uint64_t send_time_ms, uint64_t now_ms,
        ping_result_t *restrict out_result) {
    (void)pinger;
    (void)dest_addr;
    (void)identifier;
    (void)expected_sequence;
    (void)send_time_ms;
    (void)now_ms;
    if (out_result != NULL) {
        out_result->success = false;
        out_result->latency_ms = -1.0;
        snprintf(out_result->error_msg, sizeof(out_result->error_msg),
                         "simulated receive failure");
    }
    return ICMP_RECEIVE_ERROR;
}'
                        exercise_body='  monitor_state_t state;
    monitor_state_init(&state, 1200, OPENUPS_MS_PER_SEC, 0);
    state.ping.deadline_ms = 4321;
    state.ping.send_time_ms = 1000;
    state.ping.expected_sequence = 7;
    state.ping.waiting = true;

    monitor_step_result_t result = drain_icmp_replies(&ctx, 1200, &state);'
                        state_assertions='  if (state.ping.waiting || state.ping.deadline_ms != 0 ||
            state.ping.send_time_ms != 0 || state.ping.expected_sequence != 0) {
        fprintf(stderr, "reply state was not cleared after receive error\n");
        return EXIT_FAILURE;
    }'
                        expected_log='ICMP receive failed: simulated receive failure'
                        ;;
                send)
                        send_stub='bool icmp_pinger_send_echo(icmp_pinger_t *restrict pinger,
        const struct sockaddr_storage *restrict dest_addr,
        socklen_t dest_addr_len, uint16_t identifier, size_t packet_len,
        char *restrict error_msg, size_t error_size) {
    (void)pinger;
    (void)dest_addr;
    (void)dest_addr_len;
    (void)identifier;
    (void)packet_len;
    if (error_msg != NULL && error_size > 0) {
        snprintf(error_msg, error_size, "simulated send failure");
    }
    return false;
}'
                        exercise_body='  monitor_state_t state = {0};

    monitor_step_result_t result = monitor_send_ping(&ctx, &state, 1200, 64);'
                        state_assertions='  if (state.ping.waiting || state.ping.deadline_ms != 0 ||
            state.ping.send_time_ms != 0 || state.ping.expected_sequence != 0) {
        fprintf(stderr, "send failure unexpectedly armed reply tracking\n");
        return EXIT_FAILURE;
    }'
                        expected_log='Failed to send ICMP echo: simulated send failure'
                        ;;
                *)
                        echo "[ERROR] Unknown monitor harness scenario: ${scenario}" >&2
                        exit 1
                        ;;
        esac

        cat <<EOF > "${source_path}"
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/monitor.c"

static char last_log[OPENUPS_LOG_BUFFER_SIZE];
static log_level_t last_log_level = LOG_LEVEL_SILENT;
static int metrics_failure_calls = 0;

void logger_init(logger_t *restrict logger, log_level_t level,
                                 bool enable_timestamp) {
    if (logger == NULL) {
        return;
    }
    logger->level = level;
    logger->enable_timestamp = enable_timestamp;
}

void logger_log_va(const logger_t *restrict logger, log_level_t level,
                                     const char *restrict fmt, va_list ap) {
    (void)logger;
    last_log_level = level;
    vsnprintf(last_log, sizeof(last_log), fmt, ap);
}

void config_print(const config_t *restrict config,
                                    const logger_t *restrict logger) {
    (void)config;
    (void)logger;
}

bool config_log_timestamps_enabled(const config_t *restrict config) {
    (void)config;
    return false;
}

void metrics_init(metrics_t *metrics) {
    if (metrics != NULL) {
        memset(metrics, 0, sizeof(*metrics));
    }
}

void metrics_record_success(metrics_t *metrics, double latency_ms) {
    (void)metrics;
    (void)latency_ms;
}

void metrics_record_failure(metrics_t *metrics) {
    metrics_failure_calls++;
    if (metrics != NULL) {
        metrics->total_pings++;
        metrics->failed_pings++;
    }
}

double metrics_success_rate(const metrics_t *metrics) {
    (void)metrics;
    return 0.0;
}

double metrics_avg_latency(const metrics_t *metrics) {
    (void)metrics;
    return 0.0;
}

uint64_t metrics_uptime_seconds(const metrics_t *metrics) {
    (void)metrics;
    return 0;
}

bool icmp_pinger_init(icmp_pinger_t *restrict pinger, int family,
                                            char *restrict error_msg, size_t error_size) {
    (void)pinger;
    (void)family;
    (void)error_msg;
    (void)error_size;
    return true;
}

void icmp_pinger_destroy(icmp_pinger_t *restrict pinger) {
    (void)pinger;
}

${send_stub}

${receive_stub}

bool resolve_target(const char *restrict target,
                                        struct sockaddr_storage *restrict addr,
                                        socklen_t *restrict addr_len, char *restrict error_msg,
                                        size_t error_size) {
    (void)target;
    (void)addr;
    (void)addr_len;
    (void)error_msg;
    (void)error_size;
    return true;
}

shutdown_result_t shutdown_trigger(const config_t *config, logger_t *logger,
                                                                     bool use_systemctl_poweroff) {
    (void)config;
    (void)logger;
    (void)use_systemctl_poweroff;
    return SHUTDOWN_RESULT_TRIGGERED;
}

void systemd_notifier_init(systemd_notifier_t *restrict notifier) {
    if (notifier != NULL) {
        memset(notifier, 0, sizeof(*notifier));
    }
}

void systemd_notifier_destroy(systemd_notifier_t *restrict notifier) {
    (void)notifier;
}

bool systemd_notifier_is_enabled(
        const systemd_notifier_t *restrict notifier) {
    (void)notifier;
    return false;
}

bool systemd_notifier_ready(systemd_notifier_t *restrict notifier) {
    (void)notifier;
    return true;
}

bool systemd_notifier_status(systemd_notifier_t *restrict notifier,
                                                         const char *restrict status) {
    (void)notifier;
    (void)status;
    return true;
}

bool systemd_notifier_stopping(systemd_notifier_t *restrict notifier) {
    (void)notifier;
    return true;
}

bool systemd_notifier_watchdog(systemd_notifier_t *restrict notifier) {
    (void)notifier;
    return true;
}

uint64_t systemd_notifier_watchdog_interval_ms(
        const systemd_notifier_t *restrict notifier) {
    (void)notifier;
    return 0;
}

const char *shutdown_mode_to_string(shutdown_mode_t mode) {
    (void)mode;
    return "true-off";
}

void log_shutdown_countdown(const logger_t *restrict logger,
                                                        shutdown_mode_t mode, int delay_minutes) {
    (void)logger;
    (void)mode;
    (void)delay_minutes;
}

uint64_t get_monotonic_ms(void) { return 1234; }

void fill_payload_pattern(icmp_pinger_t *restrict pinger, size_t header_size,
                                                    size_t payload_size) {
    (void)pinger;
    (void)header_size;
    (void)payload_size;
}

int main(void) {
    openups_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    strcpy(ctx.config.target, "198.51.100.10");
    ctx.logger.level = LOG_LEVEL_DEBUG;

${exercise_body}
    if (result != MONITOR_STEP_ERROR) {
        fprintf(stderr, "expected runtime error result, got %d\n", result);
        return EXIT_FAILURE;
    }

${state_assertions}
    if (ctx.consecutive_fails != 0 || metrics_failure_calls != 0) {
        fprintf(stderr,
                        "runtime error was misclassified as timeout/failure accounting\n");
        return EXIT_FAILURE;
    }
    if (last_log_level != LOG_LEVEL_ERROR ||
            strstr(last_log, "${expected_log}") == NULL) {
        fprintf(stderr, "unexpected runtime error log: %s\n", last_log);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
EOF
}

write_shutdown_clock_harness() {
        local source_path="$1"

        cat <<'EOF' > "${source_path}"
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define waitpid fake_waitpid
#define nanosleep fake_nanosleep
#include "src/shutdown.c"
#undef waitpid
#undef nanosleep

static char log_buffer[4096];
static size_t log_offset = 0;

static uint64_t monotonic_values[8];
static size_t monotonic_len = 0;
static size_t monotonic_index = 0;

static pid_t waitpid_results[8];
static size_t waitpid_len = 0;
static size_t waitpid_index = 0;

static void reset_test_state(void) {
    memset(log_buffer, 0, sizeof(log_buffer));
    log_offset = 0;
    memset(monotonic_values, 0, sizeof(monotonic_values));
    monotonic_len = 0;
    monotonic_index = 0;
    memset(waitpid_results, 0, sizeof(waitpid_results));
    waitpid_len = 0;
    waitpid_index = 0;
}

void logger_log_va(const logger_t *restrict logger, log_level_t level,
                                     const char *restrict fmt, va_list ap) {
    (void)logger;
    const char *level_name = level == LOG_LEVEL_WARN ? "WARN"
                                                     : level == LOG_LEVEL_INFO ? "INFO"
                                                     : level == LOG_LEVEL_ERROR ? "ERROR"
                                                                                                            : "OTHER";
    int written = snprintf(log_buffer + log_offset,
                                                 sizeof(log_buffer) - log_offset, "[%s] ", level_name);
    if (written < 0) {
        return;
    }
    size_t prefix = (size_t)written;
    if (prefix >= sizeof(log_buffer) - log_offset) {
        log_offset = sizeof(log_buffer);
        return;
    }
    log_offset += prefix;
    written = vsnprintf(log_buffer + log_offset, sizeof(log_buffer) - log_offset,
                                            fmt, ap);
    if (written < 0) {
        return;
    }
    size_t body = (size_t)written;
    if (body >= sizeof(log_buffer) - log_offset) {
        log_offset = sizeof(log_buffer);
        return;
    }
    log_offset += body;
    if (log_offset + 1 < sizeof(log_buffer)) {
        log_buffer[log_offset++] = '\n';
        log_buffer[log_offset] = '\0';
    }
}

uint64_t get_monotonic_ms(void) {
    if (monotonic_index >= monotonic_len) {
        return UINT64_MAX;
    }
    return monotonic_values[monotonic_index++];
}

const char *shutdown_mode_to_string(shutdown_mode_t mode) {
    (void)mode;
    return "true-off";
}

pid_t fake_waitpid(pid_t pid, int *status, int options) {
    (void)pid;
    (void)status;
    (void)options;
    if (waitpid_index >= waitpid_len) {
        return 0;
    }
    return waitpid_results[waitpid_index++];
}

int fake_nanosleep(const struct timespec *req, struct timespec *rem) {
    (void)req;
    (void)rem;
    return 0;
}

static int assert_log_contains(const char *needle) {
    if (strstr(log_buffer, needle) == NULL) {
        fprintf(stderr, "missing expected log: %s\nlogs were:\n%s", needle,
                        log_buffer);
        return 0;
    }
    return 1;
}

int main(void) {
    logger_t logger = {.level = LOG_LEVEL_DEBUG, .enable_timestamp = false};

    reset_test_state();
    monotonic_values[0] = UINT64_MAX;
    monotonic_len = 1;
    waitpid_results[0] = 0;
    waitpid_len = 1;

    shutdown_result_t result =
            shutdown_observe_startup(321, "/sbin/shutdown", &logger);
    if (result != SHUTDOWN_RESULT_TRIGGERED) {
        fprintf(stderr, "expected assumed-started result on initial clock failure\n");
        return EXIT_FAILURE;
    }
    if (!assert_log_contains(
                    "Unable to confirm shutdown command startup: monotonic clock unavailable during startup observation; assuming command started")) {
        return EXIT_FAILURE;
    }
    if (strstr(log_buffer, "Shutdown command started successfully") != NULL) {
        fprintf(stderr,
                        "unexpected optimistic success log on initial clock failure\n%s",
                        log_buffer);
        return EXIT_FAILURE;
    }

    reset_test_state();
    monotonic_values[0] = 100;
    monotonic_values[1] = UINT64_MAX;
    monotonic_len = 2;
    waitpid_results[0] = 0;
    waitpid_len = 1;

    result = shutdown_observe_startup(654, "/sbin/shutdown", &logger);
    if (result != SHUTDOWN_RESULT_TRIGGERED) {
        fprintf(stderr, "expected assumed-started result before grace elapsed\n");
        return EXIT_FAILURE;
    }
    if (!assert_log_contains(
                    "Unable to confirm shutdown command startup: monotonic clock unavailable before startup grace elapsed; assuming command started")) {
        return EXIT_FAILURE;
    }
    if (strstr(log_buffer, "Shutdown command started successfully") != NULL) {
        fprintf(stderr,
                        "unexpected optimistic success log before grace elapsed\n%s",
                        log_buffer);
        return EXIT_FAILURE;
    }

    reset_test_state();
    monotonic_values[0] = UINT64_MAX - 10;
    monotonic_len = 1;

    result = shutdown_observe_startup(777, "/sbin/shutdown", &logger);
    if (result != SHUTDOWN_RESULT_TRIGGERED) {
        fprintf(stderr, "expected assumed-started result on deadline overflow\n");
        return EXIT_FAILURE;
    }
    if (!assert_log_contains(
                    "Unable to confirm shutdown command startup: startup observation deadline overflow; assuming command started")) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
EOF
}

write_monitor_shutdown_failure_harness() {
        local source_path="$1"

        cat <<'EOF' > "${source_path}"
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/monitor.c"

static char last_log[OPENUPS_LOG_BUFFER_SIZE];
static log_level_t last_log_level = LOG_LEVEL_SILENT;

void logger_init(logger_t *restrict logger, log_level_t level,
                 bool enable_timestamp) {
    if (logger == NULL) {
        return;
    }
    logger->level = level;
    logger->enable_timestamp = enable_timestamp;
}

void logger_log_va(const logger_t *restrict logger, log_level_t level,
                   const char *restrict fmt, va_list ap) {
    (void)logger;
    last_log_level = level;
    vsnprintf(last_log, sizeof(last_log), fmt, ap);
}

void config_print(const config_t *restrict config,
                  const logger_t *restrict logger) {
    (void)config;
    (void)logger;
}

bool config_log_timestamps_enabled(const config_t *restrict config) {
    (void)config;
    return false;
}

void metrics_init(metrics_t *metrics) {
    if (metrics != NULL) {
        memset(metrics, 0, sizeof(*metrics));
    }
}

void metrics_record_success(metrics_t *metrics, double latency_ms) {
    (void)metrics;
    (void)latency_ms;
}

void metrics_record_failure(metrics_t *metrics) {
    (void)metrics;
}

double metrics_success_rate(const metrics_t *metrics) {
    (void)metrics;
    return 0.0;
}

double metrics_avg_latency(const metrics_t *metrics) {
    (void)metrics;
    return 0.0;
}

uint64_t metrics_uptime_seconds(const metrics_t *metrics) {
    (void)metrics;
    return 0;
}

bool icmp_pinger_init(icmp_pinger_t *restrict pinger, int family,
                      char *restrict error_msg, size_t error_size) {
    (void)pinger;
    (void)family;
    (void)error_msg;
    (void)error_size;
    return true;
}

void icmp_pinger_destroy(icmp_pinger_t *restrict pinger) {
    (void)pinger;
}

bool icmp_pinger_send_echo(icmp_pinger_t *restrict pinger,
                           const struct sockaddr_storage *restrict dest_addr,
                           socklen_t dest_addr_len, uint16_t identifier,
                           size_t packet_len, char *restrict error_msg,
                           size_t error_size) {
    (void)pinger;
    (void)dest_addr;
    (void)dest_addr_len;
    (void)identifier;
    (void)packet_len;
    (void)error_msg;
    (void)error_size;
    return true;
}

icmp_receive_status_t icmp_pinger_receive_reply(
    const icmp_pinger_t *restrict pinger,
    const struct sockaddr_storage *restrict dest_addr, uint16_t identifier,
    uint16_t expected_sequence, uint64_t send_time_ms, uint64_t now_ms,
    ping_result_t *restrict out_result) {
    (void)pinger;
    (void)dest_addr;
    (void)identifier;
    (void)expected_sequence;
    (void)send_time_ms;
    (void)now_ms;
    (void)out_result;
    return ICMP_RECEIVE_NO_MORE;
}

bool resolve_target(const char *restrict target,
                    struct sockaddr_storage *restrict addr,
                    socklen_t *restrict addr_len, char *restrict error_msg,
                    size_t error_size) {
    (void)target;
    (void)addr;
    (void)addr_len;
    (void)error_msg;
    (void)error_size;
    return true;
}

shutdown_result_t shutdown_trigger(const config_t *config, logger_t *logger,
                                   bool use_systemctl_poweroff) {
    (void)config;
    (void)logger;
    (void)use_systemctl_poweroff;
    return SHUTDOWN_RESULT_FAILED;
}

void systemd_notifier_init(systemd_notifier_t *restrict notifier) {
    if (notifier != NULL) {
        memset(notifier, 0, sizeof(*notifier));
    }
}

void systemd_notifier_destroy(systemd_notifier_t *restrict notifier) {
    (void)notifier;
}

bool systemd_notifier_is_enabled(
    const systemd_notifier_t *restrict notifier) {
    (void)notifier;
    return false;
}

bool systemd_notifier_ready(systemd_notifier_t *restrict notifier) {
    (void)notifier;
    return true;
}

bool systemd_notifier_status(systemd_notifier_t *restrict notifier,
                             const char *restrict status) {
    (void)notifier;
    (void)status;
    return true;
}

bool systemd_notifier_stopping(systemd_notifier_t *restrict notifier) {
    (void)notifier;
    return true;
}

bool systemd_notifier_watchdog(systemd_notifier_t *restrict notifier) {
    (void)notifier;
    return true;
}

uint64_t systemd_notifier_watchdog_interval_ms(
    const systemd_notifier_t *restrict notifier) {
    (void)notifier;
    return 0;
}

const char *shutdown_mode_to_string(shutdown_mode_t mode) {
    (void)mode;
    return "true-off";
}

void log_shutdown_countdown(const logger_t *restrict logger,
                            shutdown_mode_t mode, int delay_minutes) {
    (void)logger;
    (void)mode;
    (void)delay_minutes;
}

uint64_t get_monotonic_ms(void) { return 1234; }

void fill_payload_pattern(icmp_pinger_t *restrict pinger, size_t header_size,
                          size_t payload_size) {
    (void)pinger;
    (void)header_size;
    (void)payload_size;
}

int main(void) {
    openups_ctx_t ctx;
    monitor_state_t state;
    memset(&ctx, 0, sizeof(ctx));
    monitor_state_init(&state, 1234, OPENUPS_MS_PER_SEC, 0);
    ctx.config.shutdown_mode = SHUTDOWN_MODE_TRUE_OFF;
    ctx.config.fail_threshold = 5;
    ctx.consecutive_fails = 5;
    ctx.logger.level = LOG_LEVEL_DEBUG;

    if (shutdown_fsm_handle_threshold(&ctx, &state, 1234)) {
        fprintf(stderr, "shutdown failure should not stop the monitor\n");
        return EXIT_FAILURE;
    }
    if (ctx.consecutive_fails != 5) {
        fprintf(stderr, "failure count should be preserved, got %d\n",
                ctx.consecutive_fails);
        return EXIT_FAILURE;
    }
    if (last_log_level != LOG_LEVEL_ERROR ||
        strstr(last_log,
               "Shutdown command failed; continuing monitoring with failure count preserved") == NULL) {
        fprintf(stderr, "unexpected shutdown failure log: %s\n", last_log);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
EOF
}

echo "========================================"
echo "OpenUPS 自动化测试"
echo "========================================"
echo

# ---- 编译检查 ----
echo "--- 编译 ---"
TESTS_TOTAL=$((TESTS_TOTAL + 1))
echo "[${TESTS_TOTAL}] 编译检查（无警告无错误）..."
if ! make clean > /dev/null 2>&1; then
    echo "  ❌ make clean 失败（常见原因：此前用 sudo 构建，导致 bin/ 目录产物归 root 所有）"
    exit 1
fi
BUILD_LOG="$(mktemp)"
if ! make >"${BUILD_LOG}" 2>&1; then
    echo "  ❌ 编译失败"
    cat "${BUILD_LOG}"
    rm -f "${BUILD_LOG}"
    exit 1
fi
if grep -E "^[^:]+:[0-9]+:[0-9]+: (warning|error):" "${BUILD_LOG}" > /dev/null; then
    echo "  ❌ 编译有警告或错误"
    grep -E "^[^:]+:[0-9]+:[0-9]+: (warning|error):" "${BUILD_LOG}"
    rm -f "${BUILD_LOG}"
    exit 1
fi
rm -f "${BUILD_LOG}"
echo "  ✓ 编译成功，无警告"
TESTS_PASSED=$((TESTS_PASSED + 1))

# ---- 基本功能 ----
echo ""
echo "--- 基本功能 ---"
run_test "帮助信息 (--help)" ./bin/openups --help
run_test "版本信息 (--version)" ./bin/openups --version
expect_output_match "帮助信息包含默认目标" \
    "default: 1\\.1\\.1\\.1" \
    ./bin/openups --help
expect_output_match "帮助信息包含默认间隔" \
    "default: 10" \
    ./bin/openups --help

# ---- systemd 服务配置 ----
echo ""
echo "--- systemd 服务配置 ---"
SERVICE_START_TIMEOUT_SEC=30
run_test "openups.service 不使用固定启动睡眠" \
    bash -c '! grep -Eq "^ExecStartPre=/usr/bin/sleep [0-9]+$" "$1"' _ "${ROOT_DIR}/systemd/openups.service"
run_test "openups.service 启动超时为 30 秒" \
    grep -Eq "^TimeoutStartSec=${SERVICE_START_TIMEOUT_SEC}$" "${ROOT_DIR}/systemd/openups.service"

# ---- 参数解析 ----
echo ""
echo "--- 参数解析 ---"
run_test "模式参数 --shutdown-mode dry-run" ./bin/openups --shutdown-mode dry-run --help
run_test "模式参数 --shutdown-mode true-off" ./bin/openups --shutdown-mode true-off --help
run_test "延迟倒计时参数 --shutdown-mode true-off --delay 1" ./bin/openups --shutdown-mode true-off --delay 1 --help
expect_output_match "帮助信息不再暴露独立时间戳参数" \
    "System Integration:" \
    bash -c '! "$1" --help 2>&1 | grep -q -- "--timestamp" && "$1" --help' _ ./bin/openups
expect_output_match "环境变量配置生效" \
    "Target host cannot be empty" \
    env OPENUPS_TARGET= ./bin/openups
expect_output_match "CLI 优先级高于环境变量" \
    "Timeout must be positive|Invalid value for --timeout" \
    env OPENUPS_TARGET= ./bin/openups --target 8.8.8.8 --timeout 0
expect_output_match "非法日志级别被拒绝" \
    "Invalid value for --log-level" \
    ./bin/openups --log-level garbage --help
expect_output_match "非法环境日志级别被拒绝" \
    "Invalid value for OPENUPS_LOG_LEVEL" \
    env OPENUPS_LOG_LEVEL=garbage ./bin/openups --help
expect_output_match "非法关机模式被拒绝" \
    "Invalid value for --shutdown-mode" \
    ./bin/openups --shutdown-mode garbage
expect_output_match "非法环境关机模式被拒绝" \
    "Invalid value for OPENUPS_SHUTDOWN_MODE" \
    env OPENUPS_SHUTDOWN_MODE=garbage ./bin/openups
expect_output_match "帮助选项不会掩盖后续非法参数" \
    "Invalid value for --log-level" \
    ./bin/openups --help --log-level garbage

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

# ---- 内部错误路径回归 ----
echo ""
echo "--- 内部错误路径回归 ---"

INTERNAL_TEST_DIR="$(mktemp -d)"

MONITOR_RECEIVE_TEST_SRC="${INTERNAL_TEST_DIR}/monitor_receive_runtime_error_test.c"
MONITOR_RECEIVE_TEST_BIN="${INTERNAL_TEST_DIR}/monitor_receive_runtime_error_test"
MONITOR_RECEIVE_TEST_LOG="${INTERNAL_TEST_DIR}/monitor_receive_runtime_error_test.log"
write_monitor_error_harness "${MONITOR_RECEIVE_TEST_SRC}" receive

run_internal_c_test \
        "ICMP_RECEIVE_ERROR 走 runtime error 而不是 timeout" \
    "${MONITOR_RECEIVE_TEST_SRC}" \
    "${MONITOR_RECEIVE_TEST_BIN}" \
    "${MONITOR_RECEIVE_TEST_LOG}" \
    "${ROOT_DIR}/src/monitor_state.c" \
    "${ROOT_DIR}/src/shutdown_fsm.c" \
    "${ROOT_DIR}/src/runtime_services.c"

MONITOR_SEND_TEST_SRC="${INTERNAL_TEST_DIR}/monitor_send_runtime_error_test.c"
MONITOR_SEND_TEST_BIN="${INTERNAL_TEST_DIR}/monitor_send_runtime_error_test"
MONITOR_SEND_TEST_LOG="${INTERNAL_TEST_DIR}/monitor_send_runtime_error_test.log"
write_monitor_error_harness "${MONITOR_SEND_TEST_SRC}" send

run_internal_c_test \
    "ICMP send 失败走 runtime error 而不是 timeout" \
    "${MONITOR_SEND_TEST_SRC}" \
    "${MONITOR_SEND_TEST_BIN}" \
    "${MONITOR_SEND_TEST_LOG}" \
    "${ROOT_DIR}/src/monitor_state.c" \
    "${ROOT_DIR}/src/shutdown_fsm.c" \
    "${ROOT_DIR}/src/runtime_services.c"

MONITOR_SHUTDOWN_FAILURE_TEST_SRC="${INTERNAL_TEST_DIR}/monitor_shutdown_failure_semantics_test.c"
MONITOR_SHUTDOWN_FAILURE_TEST_BIN="${INTERNAL_TEST_DIR}/monitor_shutdown_failure_semantics_test"
MONITOR_SHUTDOWN_FAILURE_TEST_LOG="${INTERNAL_TEST_DIR}/monitor_shutdown_failure_semantics_test.log"
write_monitor_shutdown_failure_harness "${MONITOR_SHUTDOWN_FAILURE_TEST_SRC}"

run_internal_c_test \
    "shutdown 命令失败时保留连续失败计数" \
    "${MONITOR_SHUTDOWN_FAILURE_TEST_SRC}" \
    "${MONITOR_SHUTDOWN_FAILURE_TEST_BIN}" \
    "${MONITOR_SHUTDOWN_FAILURE_TEST_LOG}" \
    "${ROOT_DIR}/src/monitor_state.c" \
    "${ROOT_DIR}/src/shutdown_fsm.c" \
    "${ROOT_DIR}/src/runtime_services.c"

SHUTDOWN_CLOCK_TEST_SRC="${INTERNAL_TEST_DIR}/shutdown_clock_fallback_test.c"
SHUTDOWN_CLOCK_TEST_BIN="${INTERNAL_TEST_DIR}/shutdown_clock_fallback_test"
SHUTDOWN_CLOCK_TEST_LOG="${INTERNAL_TEST_DIR}/shutdown_clock_fallback_test.log"
write_shutdown_clock_harness "${SHUTDOWN_CLOCK_TEST_SRC}"

run_internal_c_test \
        "shutdown 时钟失败路径记录 assumed-started 警告语义" \
        "${SHUTDOWN_CLOCK_TEST_SRC}" \
        "${SHUTDOWN_CLOCK_TEST_BIN}" \
        "${SHUTDOWN_CLOCK_TEST_LOG}"

rm -rf "${INTERNAL_TEST_DIR}"

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

    PHASE1_SEC="${PHASE1_SEC:-12}"
    PHASE2_SEC="${PHASE2_SEC:-14}"
    SIGNAL_TEST_SEC="${SIGNAL_TEST_SEC:-3}"

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

    launch_background_monitor() {
        local pid_file="$1"
        local out_log="$2"
        shift 2

        rm -f "${pid_file}"

        if [[ -n "${SUDO}" ]]; then
            ${SUDO} bash -c 'printf "%s\n" "$$" > "$1"; shift; exec "$@"' bash "${pid_file}" "${BIN_PATH}" "$@" >"${out_log}" 2>&1 &
        else
            "${BIN_PATH}" "$@" >"${out_log}" 2>&1 &
            printf "%s\n" "$!" >"${pid_file}"
        fi

        local wrapper_pid=$!
        local wait_count=0
        while [[ ! -s "${pid_file}" && ${wait_count} -lt 20 ]]; do
            sleep 0.1
            wait_count=$((wait_count + 1))
        done

        printf "%s\n" "${wrapper_pid}"
    }

    signal_monitor() {
        local pid_file="$1"
        local signal_number="$2"
        local monitor_pid

        monitor_pid="$(cat "${pid_file}" 2>/dev/null || true)"
        if [[ -z "${monitor_pid}" ]]; then
            return 1
        fi

        ${SUDO} kill "-${signal_number}" "${monitor_pid}" 2>/dev/null || true
        return 0
    }

    run_timeout_capture() {
        local duration_sec="$1"
        local out_log="$2"
        shift 2

        set +e
        ${SUDO} timeout "${duration_sec}s" "${BIN_PATH}" "$@" >"${out_log}" 2>&1
        local exit_code=$?
        set -e

        printf '%s\n' "${exit_code}"
    }

    echo "== Phase 1: dry-run（验证触发路径，不执行关机） =="
    run_phase "Phase 1" "${PHASE1_SEC}" "${PHASE1_LOG}" \
        --target "${TARGET_FAIL}" \
        --interval "${INTERVAL_SEC}" \
        --threshold "${THRESHOLD}" \
        --timeout "${TIMEOUT_MS}" \
        --shutdown-mode dry-run \
        --log-level debug

    echo "== Phase 2: log-only（验证安全分支） =="
    run_phase "Phase 2" "${PHASE2_SEC}" "${PHASE2_LOG}" \
        --target "${TARGET_FAIL}" \
        --interval "${INTERVAL_SEC}" \
        --threshold "${THRESHOLD}" \
        --timeout "${TIMEOUT_MS}" \
        --shutdown-mode log-only \
        --log-level debug

    # ---- 连续运行测试 ----
    echo ""
    echo "== Phase 3: 连续运行测试（信号处理 + 优雅关闭） =="

    PHASE3_LOG="${LOG_DIR}/phase3_continuous_run.log"
    PHASE4_LOG="${LOG_DIR}/phase4_sigusr1_stats.log"
    PHASE5_LOG="${LOG_DIR}/phase5_log_only_continuous.log"
    PHASE6_LOG="${LOG_DIR}/phase6_delayed_countdown.log"

    # Phase 3: SIGTERM 优雅关闭测试
    echo "[INFO] Phase 3: 连续运行 + SIGTERM 优雅关闭"
    PHASE3_PID_FILE="${LOG_DIR}/phase3.pid"
    set +e
    PHASE3_WRAPPER_PID="$(launch_background_monitor "${PHASE3_PID_FILE}" "${PHASE3_LOG}" \
        --target 127.0.0.1 \
        --interval 1 \
        --threshold 10 \
        --shutdown-mode dry-run \
        --log-level debug)"
    sleep "${SIGNAL_TEST_SEC}"
    signal_monitor "${PHASE3_PID_FILE}" 15
    wait "${PHASE3_WRAPPER_PID}" 2>/dev/null
    PHASE3_EXIT=$?
    set -e

    phase3_ping_count="$(count_lines "Ping successful" "${PHASE3_LOG}")"
    phase3_graceful="$(count_lines "stopping gracefully|monitor stopped" "${PHASE3_LOG}")"

    assert_count_at_least \
        "Phase 3" \
        "${phase3_graceful}" \
        1 \
        "连续运行 + SIGTERM 优雅关闭 (${phase3_ping_count} pings, graceful=${phase3_graceful})" \
        "连续运行测试失败 (pings=${phase3_ping_count}, graceful=${phase3_graceful})" \
        "${PHASE3_LOG}"

    # Phase 4: SIGUSR1 统计信息测试
    echo "[INFO] Phase 4: SIGUSR1 统计信息输出"
    PHASE4_PID_FILE="${LOG_DIR}/phase4.pid"
    set +e
    PHASE4_WRAPPER_PID="$(launch_background_monitor "${PHASE4_PID_FILE}" "${PHASE4_LOG}" \
        --target 127.0.0.1 \
        --interval 1 \
        --threshold 10 \
        --shutdown-mode dry-run \
        --log-level debug)"
    sleep "${SIGNAL_TEST_SEC}"
    signal_monitor "${PHASE4_PID_FILE}" 10
    sleep 1
    signal_monitor "${PHASE4_PID_FILE}" 15
    wait "${PHASE4_WRAPPER_PID}" 2>/dev/null
    set -e

    phase4_stats="$(count_lines "Statistics:" "${PHASE4_LOG}")"
    assert_count_at_least \
        "Phase 4" \
        "${phase4_stats}" \
        2 \
        "SIGUSR1 统计信息输出 (${phase4_stats} lines)" \
        "SIGUSR1 统计信息测试失败 (stats=${phase4_stats})" \
        "${PHASE4_LOG}"

    # Phase 5: log-only 模式连续运行（不退出）
    echo "[INFO] Phase 5: log-only 模式连续运行验证"
    PHASE5_DURATION=8
    PHASE5_EXIT="$(run_timeout_capture "${PHASE5_DURATION}" "${PHASE5_LOG}" \
        --target "${TARGET_FAIL}" \
        --interval 1 \
        --threshold 2 \
        --timeout "${TIMEOUT_MS}" \
        --shutdown-mode log-only \
        --log-level debug)"

    phase5_log_only="$(count_lines "LOG-ONLY mode|continuing monitoring without shutdown" "${PHASE5_LOG}")"
    assert_exit_and_count \
        "Phase 5" \
        "${PHASE5_EXIT}" \
        124 \
        "${phase5_log_only}" \
        2 \
        "log-only 模式连续运行 (${phase5_log_only} log-only triggers, kept running)" \
        "log-only 连续运行测试失败 (exit=${PHASE5_EXIT}, log_only=${phase5_log_only})" \
        "${PHASE5_LOG}"

    # Phase 6: true-off + delay 由进程内部倒计时（不应立刻退出）
    echo "[INFO] Phase 6: true-off 模式内部倒计时验证"
    PHASE6_DURATION=8
    PHASE6_EXIT="$(run_timeout_capture "${PHASE6_DURATION}" "${PHASE6_LOG}" \
        --target "${TARGET_FAIL}" \
        --interval 1 \
        --threshold 2 \
        --timeout "${TIMEOUT_MS}" \
        --shutdown-mode true-off \
        --delay 1 \
        --log-level debug)"

    phase6_countdown="$(count_lines "Starting true-off countdown|true-off countdown" "${PHASE6_LOG}")"
    assert_exit_and_count \
        "Phase 6" \
        "${PHASE6_EXIT}" \
        124 \
        "${phase6_countdown}" \
        1 \
        "true-off 模式由程序内部倒计时 (${phase6_countdown} countdown lines, kept running)" \
        "true-off 倒计时测试失败 (exit=${PHASE6_EXIT}, countdown=${phase6_countdown})" \
        "${PHASE6_LOG}"

    phase1_trigger_count="$(count_lines "Would trigger shutdown|Shutdown triggered" "${PHASE1_LOG}")"
    phase2_log_only_count="$(count_lines "LOG-ONLY mode|mode is log-only|continuing monitoring without shutdown" "${PHASE2_LOG}")"
    phase2_fail_count="$(count_lines "Ping failed" "${PHASE2_LOG}")"
    phase6_countdown_summary="${phase6_countdown}"

    {
        echo "OpenUPS 灰度验证摘要"
        echo "Run timestamp: ${RUN_TS}"
        echo "Binary: ${BIN_PATH}"
        echo
        echo "[Phase 1] dry-run"
        echo "  Trigger-related lines: ${phase1_trigger_count}"
        echo "  Log file: ${PHASE1_LOG}"
        echo
        echo "[Phase 2] log-only"
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
        echo "[Phase 6] true-off 内部倒计时"
        echo "  Countdown lines: ${phase6_countdown_summary}"
        echo "  Exit code: ${PHASE6_EXIT} (expected: 124)"
        echo "  Log file: ${PHASE6_LOG}"
        echo
        if [[ "${phase1_trigger_count}" -ge 1 && "${phase2_log_only_count}" -ge 1 && \
              "${phase3_ping_count}" -ge 2 && "${phase4_stats}" -ge 2 && \
              "${phase5_log_only}" -ge 2 && "${phase6_countdown_summary}" -ge 1 ]]; then
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
        echo "[HINT] Register the service file manually first, e.g. sudo cp systemd/openups.service /etc/systemd/system/" >&2
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
        local log_level="$2"

        ${SUDO} mkdir -p "${DROPIN_DIR}"
        cat <<DROPIN_EOF | ${SUDO} tee "${DROPIN_FILE}" >/dev/null
[Service]
Environment="OPENUPS_TARGET=${TARGET_FAIL}"
Environment="OPENUPS_INTERVAL=${INTERVAL_SEC}"
Environment="OPENUPS_THRESHOLD=${THRESHOLD}"
Environment="OPENUPS_TIMEOUT=${TIMEOUT_MS}"
Environment="OPENUPS_SHUTDOWN_MODE=${shutdown_mode}"
Environment="OPENUPS_LOG_LEVEL=${log_level}"
Environment="OPENUPS_SYSTEMD=true"
DROPIN_EOF
    }

    capture_phase() {
        local phase_name="$1"
        local shutdown_mode="$2"
        local log_level="$3"
        local duration_sec="$4"
        local out_log="$5"

        echo "[INFO] ${phase_name}: mode=${shutdown_mode}, duration=${duration_sec}s"

        write_dropin "${shutdown_mode}" "${log_level}"
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

    echo "Service: ${SERVICE_NAME}"
    echo "日志目录: ${SD_LOG_DIR}"
    echo

    capture_phase "Phase 1" "dry-run" "debug" "${SD_PHASE1_SEC}" "${SD_PHASE1_LOG}"
    capture_phase "Phase 2" "log-only" "debug" "${SD_PHASE2_SEC}" "${SD_PHASE2_LOG}"

    sd_p1_trigger="$(count_lines "Would trigger shutdown|Shutdown triggered, exiting monitor loop" "${SD_PHASE1_LOG}")"
    sd_p2_log_only="$(count_lines "mode is log-only|LOG-ONLY mode|continuing monitoring without shutdown" "${SD_PHASE2_LOG}")"
    sd_p2_fail="$(count_lines "Ping failed" "${SD_PHASE2_LOG}")"

    {
        echo "OpenUPS systemd 灰度验证摘要"
        echo "Run timestamp: ${RUN_TS}"
        echo "Service: ${SERVICE_NAME}"
        echo
        echo "[Phase 1] dry-run"
        echo "  Trigger-related lines: ${sd_p1_trigger}"
        echo "  Log file: ${SD_PHASE1_LOG}"
        echo
        echo "[Phase 2] log-only"
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
