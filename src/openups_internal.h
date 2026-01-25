#ifndef OPENUPS_INTERNAL_H
#define OPENUPS_INTERNAL_H

/* OpenUPS 内部实现头文件。
 * 约定：仅供本仓库内部 .c 使用；对外稳定 API 在 openups.h。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>

/* ===== common ===== */

/* C23 checked arithmetic（stdckdint.h）；不支持时提供等价回退实现。 */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
#include <stdckdint.h>
#else
static inline bool openups_ckd_add_u64(uint64_t* result, uint64_t a, uint64_t b)
{
    if (result == NULL) {
        return true;
    }
    if (b > UINT64_MAX - a) {
        return true;
    }
    *result = a + b;
    return false;
}

static inline bool openups_ckd_mul_u64(uint64_t* result, uint64_t a, uint64_t b)
{
    if (result == NULL) {
        return true;
    }
    if (a != 0 && b > UINT64_MAX / a) {
        return true;
    }
    *result = a * b;
    return false;
}

#define ckd_add(result, a, b) openups_ckd_add_u64((result), (uint64_t)(a), (uint64_t)(b))
#define ckd_mul(result, a, b) openups_ckd_mul_u64((result), (uint64_t)(a), (uint64_t)(b))
#endif

#define VERSION "1.2.0"
#define PROGRAM_NAME "openups"

[[nodiscard]] uint64_t get_timestamp_ms(void);
[[nodiscard]] uint64_t get_monotonic_ms(void);
[[nodiscard]] char* get_timestamp_str(char* restrict buffer, size_t size);

[[nodiscard]] bool get_env_bool(const char* restrict name, bool default_value);
[[nodiscard]] int get_env_int(const char* restrict name, int default_value);

[[nodiscard]] bool is_safe_path(const char* restrict path);

/* ===== logger ===== */

typedef enum {
    LOG_LEVEL_SILENT = -1,
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_WARN = 1,
    LOG_LEVEL_INFO = 2,
    LOG_LEVEL_DEBUG = 3
} log_level_t;

typedef struct {
    log_level_t level;
    bool enable_timestamp;
} logger_t;

void logger_init(logger_t* restrict logger, log_level_t level, bool enable_timestamp);
void logger_destroy(logger_t* restrict logger);

void logger_debug(logger_t* restrict logger, const char* restrict fmt, ...)
    __attribute__((format(printf, 2, 3)));
void logger_info(logger_t* restrict logger, const char* restrict fmt, ...)
    __attribute__((format(printf, 2, 3)));
void logger_warn(logger_t* restrict logger, const char* restrict fmt, ...)
    __attribute__((format(printf, 2, 3)));
void logger_error(logger_t* restrict logger, const char* restrict fmt, ...)
    __attribute__((format(printf, 2, 3)));

[[nodiscard]] const char* log_level_to_string(log_level_t level);
[[nodiscard]] log_level_t string_to_log_level(const char* restrict str);

/* ===== config ===== */

typedef enum {
    SHUTDOWN_MODE_IMMEDIATE,
    SHUTDOWN_MODE_DELAYED,
    SHUTDOWN_MODE_LOG_ONLY
} shutdown_mode_t;

typedef struct {
    char target[256];
    int interval_sec;
    int fail_threshold;
    int timeout_ms;
    int packet_size;
    int max_retries;
    bool use_ipv6;

    shutdown_mode_t shutdown_mode;
    int delay_minutes;
    bool dry_run;

    bool enable_timestamp;
    log_level_t log_level;

    bool enable_systemd;
    bool enable_watchdog;
} config_t;

void config_init_default(config_t* restrict config);
void config_load_from_env(config_t* restrict config);
[[nodiscard]] bool config_load_from_cmdline(config_t* restrict config, int argc,
                                            char** restrict argv);
[[nodiscard]] bool config_validate(const config_t* restrict config, char* restrict error_msg,
                                   size_t error_size);
void config_print(const config_t* restrict config);
void config_print_usage(void);
void config_print_version(void);

[[nodiscard]] const char* shutdown_mode_to_string(shutdown_mode_t mode);
[[nodiscard]] shutdown_mode_t string_to_shutdown_mode(const char* restrict str);
[[nodiscard]] bool shutdown_mode_parse(const char* restrict str,
                                       shutdown_mode_t* restrict out_mode);

/* ===== icmp ===== */

typedef struct {
    bool success;
    double latency_ms;
    char error_msg[256];
} ping_result_t;

typedef struct {
    bool use_ipv6;
    int sockfd;

    uint16_t sequence;
    uint8_t* send_buf;
    size_t send_buf_capacity;
    size_t payload_filled_size;

    bool cached_target_valid;
    char cached_target[256];
    struct sockaddr_storage cached_addr;
    socklen_t cached_addr_len;
} icmp_pinger_t;

typedef void (*icmp_tick_fn)(void* user_data);
typedef bool (*icmp_should_stop_fn)(void* user_data);

[[nodiscard]] bool icmp_pinger_init(icmp_pinger_t* restrict pinger, bool use_ipv6,
                                    char* restrict error_msg, size_t error_size);
void icmp_pinger_destroy(icmp_pinger_t* restrict pinger);

[[nodiscard]] ping_result_t icmp_pinger_ping(icmp_pinger_t* restrict pinger,
                                             const char* restrict target, int timeout_ms,
                                             int packet_size);
[[nodiscard]] ping_result_t icmp_pinger_ping_ex(icmp_pinger_t* restrict pinger,
                                                const char* restrict target, int timeout_ms,
                                                int packet_size, icmp_tick_fn tick,
                                                void* tick_user_data,
                                                icmp_should_stop_fn should_stop,
                                                void* stop_user_data);

/* ===== systemd ===== */

typedef struct {
    bool enabled;
    int sockfd;
    uint64_t watchdog_usec;
    struct sockaddr_un addr;
    socklen_t addr_len;

    uint64_t last_watchdog_ms;
    uint64_t last_status_ms;
    char last_status[256];
} systemd_notifier_t;

void systemd_notifier_init(systemd_notifier_t* restrict notifier);
void systemd_notifier_destroy(systemd_notifier_t* restrict notifier);
[[nodiscard]] bool systemd_notifier_is_enabled(const systemd_notifier_t* restrict notifier);
[[nodiscard]] bool systemd_notifier_ready(systemd_notifier_t* restrict notifier);
[[nodiscard]] bool systemd_notifier_status(systemd_notifier_t* restrict notifier,
                                           const char* restrict status);
[[nodiscard]] bool systemd_notifier_stopping(systemd_notifier_t* restrict notifier);
[[nodiscard]] bool systemd_notifier_watchdog(systemd_notifier_t* restrict notifier);
[[nodiscard]] uint64_t systemd_notifier_watchdog_interval_ms(
    const systemd_notifier_t* restrict notifier);

/* ===== metrics ===== */

typedef struct {
    uint64_t total_pings;
    uint64_t successful_pings;
    uint64_t failed_pings;
    double min_latency;
    double max_latency;
    double total_latency;
    uint64_t start_time_ms;
} metrics_t;

void metrics_init(metrics_t* metrics);
void metrics_record_success(metrics_t* metrics, double latency_ms);
void metrics_record_failure(metrics_t* metrics);

[[nodiscard]] double metrics_success_rate(const metrics_t* metrics);
[[nodiscard]] double metrics_avg_latency(const metrics_t* metrics);
[[nodiscard]] uint64_t metrics_uptime_seconds(const metrics_t* metrics);

/* ===== shutdown ===== */

void shutdown_trigger(const config_t* config, logger_t* logger, bool use_systemctl);

/* ===== context ===== */

typedef struct openups_context {
    volatile sig_atomic_t stop_flag;
    volatile sig_atomic_t print_stats_flag;
    int consecutive_fails;

    config_t config;
    logger_t logger;
    icmp_pinger_t pinger;
    systemd_notifier_t systemd;
    metrics_t metrics;

    bool systemd_enabled;
    uint64_t watchdog_interval_ms;

    uint64_t last_ping_time_ms;
    uint64_t start_time_ms;
} openups_ctx_t;

[[nodiscard]] bool openups_ctx_init(openups_ctx_t* restrict ctx, int argc,
                                    char** restrict argv, char* restrict error_msg,
                                    size_t error_size);
void openups_ctx_destroy(openups_ctx_t* restrict ctx);
[[nodiscard]] int openups_ctx_run(openups_ctx_t* restrict ctx);
void openups_ctx_print_stats(openups_ctx_t* restrict ctx);
[[nodiscard]] bool openups_ctx_ping_once(openups_ctx_t* restrict ctx,
                                         ping_result_t* restrict result);
void openups_ctx_sleep_interruptible(openups_ctx_t* restrict ctx, int seconds);

#endif /* OPENUPS_INTERNAL_H */
