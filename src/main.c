/* ============================================================
 * Standard library includes (deduplicated)
 * ============================================================ */
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <netinet/icmp6.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef _WIN32
#error "This program currently targets Linux."
#endif

/* ============================================================
 * Version / name defines
 * ============================================================ */
#define OPENUPS_VERSION "1.4.0"
#define OPENUPS_PROGRAM_NAME "openups"

/* ============================================================
 * Compiler hint macros
 * ============================================================ */
#if defined(__GNUC__) || defined(__clang__)
#define OPENUPS_LIKELY(x) __builtin_expect(!!(x), 1)
#define OPENUPS_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define OPENUPS_HOT __attribute__((hot))
#define OPENUPS_COLD __attribute__((cold))
#define OPENUPS_PURE __attribute__((pure))
#define OPENUPS_CONST __attribute__((const))
#define OPENUPS_FLATTEN __attribute__((flatten))
#else
#define OPENUPS_LIKELY(x) (x)
#define OPENUPS_UNLIKELY(x) (x)
#define OPENUPS_HOT
#define OPENUPS_COLD
#define OPENUPS_PURE
#define OPENUPS_CONST
#define OPENUPS_FLATTEN
#endif

/* ============================================================
 * C23 checked arithmetic fallback
 * ============================================================ */
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

/* ============================================================
 * AVX2 feature detection
 * ============================================================ */
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
#include <immintrin.h>
#define OPENUPS_CAN_USE_CPU_SUPPORTS 1
#else
#define OPENUPS_CAN_USE_CPU_SUPPORTS 0
#endif

/* ============================================================
 * Type definitions
 * ============================================================ */

/* --- log_level_t --- */
typedef enum {
    LOG_LEVEL_SILENT = -1,
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_WARN = 1,
    LOG_LEVEL_INFO = 2,
    LOG_LEVEL_DEBUG = 3
} log_level_t;

/* --- logger_t --- */
typedef struct {
    log_level_t level;
    bool enable_timestamp;
} logger_t;

/* --- metrics_t --- */
typedef struct {
    uint64_t total_pings;
    uint64_t successful_pings;
    uint64_t failed_pings;
    double total_latency;
    double min_latency;
    double max_latency;
    uint64_t start_time_ms;
} metrics_t;

/* --- shutdown_mode_t --- */
typedef enum {
    SHUTDOWN_MODE_IMMEDIATE,
    SHUTDOWN_MODE_DELAYED,
    SHUTDOWN_MODE_LOG_ONLY
} shutdown_mode_t;

/* --- config_t --- */
typedef struct {
    char target[256];
    int interval_sec;
    int fail_threshold;
    int timeout_ms;
    int payload_size;
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

/* --- ping_result_t --- */
typedef struct {
    bool success;
    double latency_ms;
    char error_msg[256];
} ping_result_t;

/* --- icmp_pinger_t --- */
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

/* --- systemd_notifier_t --- */
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

/* --- openups_ctx_t --- */
typedef struct openups_context {
    volatile sig_atomic_t stop_flag;
    volatile sig_atomic_t print_stats_flag;
    int consecutive_fails;

    bool systemd_enabled;
    uint64_t watchdog_interval_ms;
    uint64_t last_ping_time_ms;
    uint64_t start_time_ms;

    config_t config;
    logger_t logger;
    metrics_t metrics;
    icmp_pinger_t pinger;
    systemd_notifier_t systemd;
} openups_ctx_t;

/* ============================================================
 * Forward declarations
 * ============================================================ */
static OPENUPS_CONST const char* log_level_to_string(log_level_t level);
static OPENUPS_PURE log_level_t string_to_log_level(const char* restrict str);
static OPENUPS_CONST const char* shutdown_mode_to_string(shutdown_mode_t mode);
static bool shutdown_mode_parse(const char* restrict str, shutdown_mode_t* restrict out_mode);
static OPENUPS_PURE bool is_safe_path(const char* restrict path);
static uint64_t get_monotonic_ms(void);
static OPENUPS_COLD void config_print_usage(void);
static OPENUPS_COLD void config_print_version(void);

/* ============================================================
 * Compile-time assertions
 * ============================================================ */
static_assert(sizeof(uint64_t) == 8, "uint64_t must be 8 bytes");
static_assert(sizeof(time_t) >= 4, "time_t must be at least 4 bytes");
static_assert(sizeof(struct icmphdr) >= 8, "icmphdr must be at least 8 bytes");
static_assert(sizeof(struct icmp6_hdr) >= 8, "icmp6_hdr must be at least 8 bytes");
static_assert(sizeof(sig_atomic_t) >= sizeof(int), "sig_atomic_t must be at least int size");
/* ============================================================
 * Base utility functions
 * ============================================================ */

__attribute__((unused))
static uint64_t get_timestamp_ms(void)
{
    struct timespec ts;
    if (OPENUPS_UNLIKELY(clock_gettime(CLOCK_REALTIME, &ts) != 0)) {
        return UINT64_MAX;
    }
    uint64_t seconds_ms = 0;
    if (OPENUPS_UNLIKELY(ckd_mul(&seconds_ms, (uint64_t)ts.tv_sec, UINT64_C(1000)))) {
        return UINT64_MAX;
    }
    uint64_t nsec_ms = (uint64_t)ts.tv_nsec / UINT64_C(1000000);
    uint64_t timestamp = 0;
    if (OPENUPS_UNLIKELY(ckd_add(&timestamp, seconds_ms, nsec_ms))) {
        return UINT64_MAX;
    }
    return timestamp;
}

static uint64_t get_monotonic_ms(void)
{
    struct timespec ts;
    if (OPENUPS_UNLIKELY(clock_gettime(CLOCK_MONOTONIC, &ts) != 0)) {
        return UINT64_MAX;
    }
    uint64_t seconds_ms = 0;
    if (OPENUPS_UNLIKELY(ckd_mul(&seconds_ms, (uint64_t)ts.tv_sec, UINT64_C(1000)))) {
        return UINT64_MAX;
    }
    uint64_t nsec_ms = (uint64_t)ts.tv_nsec / UINT64_C(1000000);
    uint64_t timestamp = 0;
    if (OPENUPS_UNLIKELY(ckd_add(&timestamp, seconds_ms, nsec_ms))) {
        return UINT64_MAX;
    }
    return timestamp;
}

static char* get_timestamp_str(char* restrict buffer, size_t size)
{
    if (OPENUPS_UNLIKELY(buffer == NULL || size == 0)) {
        return NULL;
    }
    struct timespec ts;
    if (OPENUPS_UNLIKELY(clock_gettime(CLOCK_REALTIME, &ts) != 0)) {
        buffer[0] = '\0';
        return buffer;
    }
    struct tm tm_info;
    if (OPENUPS_UNLIKELY(localtime_r(&ts.tv_sec, &tm_info) == NULL)) {
        buffer[0] = '\0';
        return buffer;
    }
    snprintf(buffer, size, "%04d-%02d-%02d %02d:%02d:%02d.%03ld", tm_info.tm_year + 1900,
             tm_info.tm_mon + 1, tm_info.tm_mday, tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec,
             ts.tv_nsec / 1000000);
    return buffer;
}

static bool get_env_bool(const char* restrict name, bool default_value)
{
    if (name == NULL) {
        return default_value;
    }
    const char* value = getenv(name);
    if (value == NULL) {
        return default_value;
    }
    if (strcasecmp(value, "true") == 0) {
        return true;
    }
    if (strcasecmp(value, "false") == 0) {
        return false;
    }
    return default_value;
}

static int get_env_int(const char* restrict name, int default_value)
{
    if (name == NULL) {
        return default_value;
    }
    const char* value = getenv(name);
    if (value == NULL) {
        return default_value;
    }
    char* endptr = NULL;
    errno = 0;
    long result = strtol(value, &endptr, 10);
    if (errno != 0 || *endptr != '\0' || result > INT_MAX || result < INT_MIN) {
        return default_value;
    }
    return (int)result;
}

static OPENUPS_PURE bool is_safe_path(const char* restrict path)
{
    if (OPENUPS_UNLIKELY(path == NULL || *path == '\0')) {
        return false;
    }
    static const bool dangerous[256] = {
        [';'] = true, ['|'] = true, ['&'] = true, ['$'] = true,  ['`'] = true,
        ['<'] = true, ['>'] = true, ['"'] = true, ['\''] = true, ['('] = true,
        [')'] = true, ['{'] = true, ['}'] = true, ['['] = true,  [']'] = true,
        ['!'] = true, ['\\'] = true, ['*'] = true, ['?'] = true,
    };
    for (const char* p = path; *p != '\0'; ++p) {
        if (OPENUPS_UNLIKELY(dangerous[(unsigned char)*p])) {
            return false;
        }
    }
    if (OPENUPS_UNLIKELY(strstr(path, "..") != NULL)) {
        return false;
    }
    return true;
}
/* ============================================================
 * Logger functions
 * ============================================================ */

static void logger_init(logger_t* restrict logger, log_level_t level, bool enable_timestamp)
{
    if (logger == NULL) {
        return;
    }
    logger->level = level;
    logger->enable_timestamp = enable_timestamp;
}

static void logger_destroy(logger_t* restrict logger)
{
    if (logger == NULL) {
        return;
    }
}

static OPENUPS_CONST const char* log_level_to_string(log_level_t level)
{
    switch (level) {
        case LOG_LEVEL_SILENT: return "SILENT";
        case LOG_LEVEL_ERROR: return "ERROR";
        case LOG_LEVEL_WARN: return "WARN";
        case LOG_LEVEL_INFO: return "INFO";
        case LOG_LEVEL_DEBUG: return "DEBUG";
        default: return "UNKNOWN";
    }
}

static OPENUPS_PURE log_level_t string_to_log_level(const char* restrict str)
{
    if (str == NULL) {
        return LOG_LEVEL_INFO;
    }
    if (strcasecmp(str, "silent") == 0 || strcasecmp(str, "none") == 0) {
        return LOG_LEVEL_SILENT;
    }
    if (strcasecmp(str, "error") == 0) return LOG_LEVEL_ERROR;
    if (strcasecmp(str, "warn") == 0) return LOG_LEVEL_WARN;
    if (strcasecmp(str, "info") == 0) return LOG_LEVEL_INFO;
    if (strcasecmp(str, "debug") == 0) return LOG_LEVEL_DEBUG;
    return LOG_LEVEL_INFO;
}

static void log_message(logger_t* restrict logger, log_level_t level, const char* restrict msg)
{
    if (OPENUPS_UNLIKELY(logger == NULL || msg == NULL)) {
        return;
    }
    if (OPENUPS_UNLIKELY(logger->level == LOG_LEVEL_SILENT || level > logger->level)) {
        return;
    }
    char timestamp[64];
    if (logger->enable_timestamp) {
        if (get_timestamp_str(timestamp, sizeof(timestamp)) == NULL) {
            timestamp[0] = '\0';
        }
    }
    const char* level_str = log_level_to_string(level);
    if (logger->enable_timestamp) {
        fprintf(stderr, "[%s] [%s] %s\n", timestamp, level_str, msg);
    } else {
        fprintf(stderr, "[%s] %s\n", level_str, msg);
    }
}

OPENUPS_HOT static void logger_debug(logger_t* restrict logger, const char* restrict fmt, ...)
    __attribute__((format(printf, 2, 3)));
OPENUPS_HOT static void logger_debug(logger_t* restrict logger, const char* restrict fmt, ...)
{
    if (OPENUPS_LIKELY(logger == NULL || fmt == NULL || logger->level < LOG_LEVEL_DEBUG)) {
        return;
    }
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    log_message(logger, LOG_LEVEL_DEBUG, buffer);
}

OPENUPS_HOT static void logger_info(logger_t* restrict logger, const char* restrict fmt, ...)
    __attribute__((format(printf, 2, 3)));
OPENUPS_HOT static void logger_info(logger_t* restrict logger, const char* restrict fmt, ...)
{
    if (logger == NULL || fmt == NULL || logger->level < LOG_LEVEL_INFO) {
        return;
    }
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    log_message(logger, LOG_LEVEL_INFO, buffer);
}

static void logger_warn(logger_t* restrict logger, const char* restrict fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void logger_warn(logger_t* restrict logger, const char* restrict fmt, ...)
{
    if (logger == NULL || fmt == NULL || logger->level < LOG_LEVEL_WARN) {
        return;
    }
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    log_message(logger, LOG_LEVEL_WARN, buffer);
}

OPENUPS_COLD static void logger_error(logger_t* restrict logger, const char* restrict fmt, ...)
    __attribute__((format(printf, 2, 3)));
OPENUPS_COLD static void logger_error(logger_t* restrict logger, const char* restrict fmt, ...)
{
    if (OPENUPS_UNLIKELY(logger == NULL || fmt == NULL || logger->level < LOG_LEVEL_ERROR)) {
        return;
    }
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    log_message(logger, LOG_LEVEL_ERROR, buffer);
}
/* ============================================================
 * Metrics functions
 * ============================================================ */

static void metrics_init(metrics_t* metrics)
{
    if (metrics == NULL) {
        return;
    }
    metrics->total_pings = 0;
    metrics->successful_pings = 0;
    metrics->failed_pings = 0;
    metrics->total_latency = 0.0;
    metrics->min_latency = -1.0;
    metrics->max_latency = -1.0;
    metrics->start_time_ms = get_monotonic_ms();
}

OPENUPS_HOT static void metrics_record_success(metrics_t* metrics, double latency_ms)
{
    if (OPENUPS_UNLIKELY(metrics == NULL)) {
        return;
    }
    metrics->total_pings++;
    metrics->successful_pings++;
    metrics->total_latency += latency_ms;
    if (OPENUPS_UNLIKELY(metrics->min_latency < 0.0) || latency_ms < metrics->min_latency) {
        metrics->min_latency = latency_ms;
    }
    if (OPENUPS_UNLIKELY(metrics->max_latency < 0.0) || latency_ms > metrics->max_latency) {
        metrics->max_latency = latency_ms;
    }
}

OPENUPS_HOT static void metrics_record_failure(metrics_t* metrics)
{
    if (OPENUPS_UNLIKELY(metrics == NULL)) {
        return;
    }
    metrics->total_pings++;
    metrics->failed_pings++;
}

static OPENUPS_PURE double metrics_success_rate(const metrics_t* metrics)
{
    if (metrics == NULL || metrics->total_pings == 0) {
        return 0.0;
    }
    return (double)metrics->successful_pings / (double)metrics->total_pings * 100.0;
}

static OPENUPS_PURE double metrics_avg_latency(const metrics_t* metrics)
{
    if (metrics == NULL || metrics->successful_pings == 0) {
        return 0.0;
    }
    return metrics->total_latency / (double)metrics->successful_pings;
}

static uint64_t metrics_uptime_seconds(const metrics_t* metrics)
{
    if (metrics == NULL) {
        return 0;
    }
    uint64_t now_ms = get_monotonic_ms();
    if (now_ms == UINT64_MAX || metrics->start_time_ms == UINT64_MAX ||
        now_ms < metrics->start_time_ms) {
        return 0;
    }
    return (now_ms - metrics->start_time_ms) / 1000ULL;
}
/* ============================================================
 * Config functions
 * ============================================================ */

static bool parse_bool_arg(const char* arg, bool* out_value)
{
    if (OPENUPS_UNLIKELY(out_value == NULL)) {
        return false;
    }
    if (arg == NULL) {
        *out_value = true;
        return true;
    }
    if (strcasecmp(arg, "true") == 0) {
        *out_value = true;
        return true;
    }
    if (strcasecmp(arg, "false") == 0) {
        *out_value = false;
        return true;
    }
    return false;
}

static bool parse_int_arg(const char* arg, int* out_value, int min_value, int max_value,
                          const char* restrict name)
{
    if (arg == NULL || out_value == NULL || name == NULL) {
        return false;
    }
    errno = 0;
    char* endptr = NULL;
    long value = strtol(arg, &endptr, 10);
    if (errno != 0 || endptr == arg || *endptr != '\0') {
        fprintf(stderr, "Invalid value for %s: %s (expect integer)\n", name, arg);
        return false;
    }
    if (value < min_value || value > max_value) {
        fprintf(stderr, "Invalid value for %s: %s (range %d..%d)\n", name, arg, min_value,
                max_value);
        return false;
    }
    *out_value = (int)value;
    return true;
}

static bool is_valid_ip_literal(const char* restrict target, bool use_ipv6)
{
    if (target == NULL || target[0] == '\0') {
        return false;
    }
    unsigned char buf[sizeof(struct in6_addr)];
    int family = use_ipv6 ? AF_INET6 : AF_INET;
    return inet_pton(family, target, buf) == 1;
}

static OPENUPS_CONST const char* shutdown_mode_to_string(shutdown_mode_t mode)
{
    switch (mode) {
        case SHUTDOWN_MODE_IMMEDIATE: return "immediate";
        case SHUTDOWN_MODE_DELAYED: return "delayed";
        case SHUTDOWN_MODE_LOG_ONLY: return "log-only";
        default: return "unknown";
    }
}

static bool shutdown_mode_parse(const char* restrict str, shutdown_mode_t* restrict out_mode)
{
    if (out_mode == NULL) return false;
    if (str == NULL) return false;
    if (strcasecmp(str, "immediate") == 0) { *out_mode = SHUTDOWN_MODE_IMMEDIATE; return true; }
    if (strcasecmp(str, "delayed") == 0) { *out_mode = SHUTDOWN_MODE_DELAYED; return true; }
    if (strcasecmp(str, "log-only") == 0) { *out_mode = SHUTDOWN_MODE_LOG_ONLY; return true; }
    return false;
}

__attribute__((unused))
static OPENUPS_PURE shutdown_mode_t string_to_shutdown_mode(const char* restrict str)
{
    if (str == NULL) return SHUTDOWN_MODE_IMMEDIATE;
    shutdown_mode_t parsed_mode;
    if (shutdown_mode_parse(str, &parsed_mode)) return parsed_mode;
    return SHUTDOWN_MODE_IMMEDIATE;
}

static void config_init_default(config_t* restrict config)
{
    if (config == NULL) {
        return;
    }
    snprintf(config->target, sizeof(config->target), "1.1.1.1");
    config->interval_sec = 10;
    config->fail_threshold = 5;
    config->timeout_ms = 2000;
    config->payload_size = 56;
    config->max_retries = 2;
    config->use_ipv6 = false;
    config->shutdown_mode = SHUTDOWN_MODE_IMMEDIATE;
    config->delay_minutes = 1;
    config->dry_run = true;
    config->enable_timestamp = true;
    config->log_level = LOG_LEVEL_INFO;
    config->enable_systemd = true;
    config->enable_watchdog = true;
}

static void config_load_from_env(config_t* restrict config)
{
    if (config == NULL) {
        return;
    }
    const char* value;
    value = getenv("OPENUPS_TARGET");
    if (value) {
        snprintf(config->target, sizeof(config->target), "%s", value);
    }
    config->interval_sec = get_env_int("OPENUPS_INTERVAL", config->interval_sec);
    config->fail_threshold = get_env_int("OPENUPS_THRESHOLD", config->fail_threshold);
    config->timeout_ms = get_env_int("OPENUPS_TIMEOUT", config->timeout_ms);
    config->payload_size = get_env_int("OPENUPS_PAYLOAD_SIZE", config->payload_size);
    config->max_retries = get_env_int("OPENUPS_RETRIES", config->max_retries);
    config->use_ipv6 = get_env_bool("OPENUPS_IPV6", config->use_ipv6);
    value = getenv("OPENUPS_SHUTDOWN_MODE");
    if (value != NULL) {
        shutdown_mode_t parsed_mode;
        if (shutdown_mode_parse(value, &parsed_mode)) {
            config->shutdown_mode = parsed_mode;
        }
    }
    config->delay_minutes = get_env_int("OPENUPS_DELAY_MINUTES", config->delay_minutes);
    config->dry_run = get_env_bool("OPENUPS_DRY_RUN", config->dry_run);
    value = getenv("OPENUPS_LOG_LEVEL");
    if (value != NULL) {
        config->log_level = string_to_log_level(value);
    }
    config->enable_systemd = get_env_bool("OPENUPS_SYSTEMD", config->enable_systemd);
    config->enable_watchdog = get_env_bool("OPENUPS_WATCHDOG", config->enable_watchdog);
    config->enable_timestamp = get_env_bool("OPENUPS_TIMESTAMP", config->enable_timestamp);
}

static bool config_load_from_cmdline(config_t* restrict config, int argc, char** restrict argv)
{
    if (config == NULL || argv == NULL) {
        return false;
    }
    static struct option long_options[] = {
        {"target", required_argument, 0, 't'},
        {"interval", required_argument, 0, 'i'},
        {"threshold", required_argument, 0, 'n'},
        {"timeout", required_argument, 0, 'w'},
        {"payload-size", required_argument, 0, 's'},
        {"retries", required_argument, 0, 'r'},
        {"ipv6", optional_argument, 0, '6'},
        {"shutdown-mode", required_argument, 0, 'S'},
        {"delay", required_argument, 0, 'D'},
        {"dry-run", optional_argument, 0, 'd'},
        {"log-level", required_argument, 0, 'L'},
        {"timestamp", optional_argument, 0, 'T'},
        {"systemd", optional_argument, 0, 'M'},
        {"watchdog", optional_argument, 0, 'W'},
        {"version", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    int c;
    int option_index = 0;
    const char* optstring = "t:i:n:w:s:r:6::S:D:d::L:T::M::W::vh";
    while ((c = getopt_long(argc, argv, optstring, long_options, &option_index)) != -1) {
        switch (c) {
            case 't': snprintf(config->target, sizeof(config->target), "%s", optarg); break;
            case 'i':
                if (!parse_int_arg(optarg, &config->interval_sec, 1, INT_MAX, "--interval")) return false;
                break;
            case 'n':
                if (!parse_int_arg(optarg, &config->fail_threshold, 1, INT_MAX, "--threshold")) return false;
                break;
            case 'w':
                if (!parse_int_arg(optarg, &config->timeout_ms, 1, INT_MAX, "--timeout")) return false;
                break;
            case 's':
                if (!parse_int_arg(optarg, &config->payload_size, 0, 65507, "--payload-size")) return false;
                break;
            case 'r':
                if (!parse_int_arg(optarg, &config->max_retries, 0, INT_MAX, "--retries")) return false;
                break;
            case 'S': {
                shutdown_mode_t parsed_mode;
                if (!shutdown_mode_parse(optarg, &parsed_mode)) {
                    fprintf(stderr, "Invalid value for --shutdown-mode: %s (use immediate|delayed|log-only)\n",
                            optarg ? optarg : "<empty>");
                    return false;
                }
                config->shutdown_mode = parsed_mode;
            } break;
            case 'D':
                if (!parse_int_arg(optarg, &config->delay_minutes, 1, INT_MAX, "--delay")) return false;
                break;
            case 'L': config->log_level = string_to_log_level(optarg); break;
            case '6':
                if (!parse_bool_arg(optarg, &config->use_ipv6)) {
                    fprintf(stderr, "Invalid value for --ipv6: %s (use true|false)\n", optarg ? optarg : "<empty>");
                    return false;
                }
                break;
            case 'd':
                if (!parse_bool_arg(optarg, &config->dry_run)) {
                    fprintf(stderr, "Invalid value for --dry-run: %s (use true|false)\n", optarg ? optarg : "<empty>");
                    return false;
                }
                break;
            case 'T':
                if (!parse_bool_arg(optarg, &config->enable_timestamp)) {
                    fprintf(stderr, "Invalid value for --timestamp: %s (use true|false)\n", optarg ? optarg : "<empty>");
                    return false;
                }
                break;
            case 'M':
                if (!parse_bool_arg(optarg, &config->enable_systemd)) {
                    fprintf(stderr, "Invalid value for --systemd: %s (use true|false)\n", optarg ? optarg : "<empty>");
                    return false;
                }
                break;
            case 'W':
                if (!parse_bool_arg(optarg, &config->enable_watchdog)) {
                    fprintf(stderr, "Invalid value for --watchdog: %s (use true|false)\n", optarg ? optarg : "<empty>");
                    return false;
                }
                break;
            case 'v': config_print_version(); exit(0);
            case 'h': config_print_usage(); exit(0);
            case '?': return false;
            default: return false;
        }
    }
    if (optind < argc) {
        fprintf(stderr, "Unexpected argument: %s\n", argv[optind]);
        return false;
    }
    return true;
}

static bool config_validate(const config_t* restrict config, char* restrict error_msg, size_t error_size)
{
    if (config == NULL || error_msg == NULL || error_size == 0) {
        return false;
    }
    if (config->target[0] == '\0') {
        snprintf(error_msg, error_size, "Target host cannot be empty");
        return false;
    }
    if (!is_valid_ip_literal(config->target, config->use_ipv6)) {
        snprintf(error_msg, error_size, "Target must be a valid %s address (DNS is disabled)",
                 config->use_ipv6 ? "IPv6" : "IPv4");
        return false;
    }
    if (config->interval_sec <= 0) {
        snprintf(error_msg, error_size, "Interval must be positive");
        return false;
    }
    if (config->fail_threshold <= 0) {
        snprintf(error_msg, error_size, "Failure threshold must be positive");
        return false;
    }
    if (config->timeout_ms <= 0) {
        snprintf(error_msg, error_size, "Timeout must be positive");
        return false;
    }
    const int max_payload = config->use_ipv6 ? 65487 : 65507;
    if (config->payload_size < 0 || config->payload_size > max_payload) {
        snprintf(error_msg, error_size, "Payload size must be between 0 and %d", max_payload);
        return false;
    }
    if (config->max_retries < 0) {
        snprintf(error_msg, error_size, "Max retries cannot be negative");
        return false;
    }
    if (config->shutdown_mode == SHUTDOWN_MODE_DELAYED) {
        if (config->delay_minutes <= 0) {
            snprintf(error_msg, error_size, "Delay minutes must be positive for delayed mode");
            return false;
        }
        if (config->delay_minutes > 60 * 24 * 365) {
            snprintf(error_msg, error_size, "Delay minutes too large (max 525600)");
            return false;
        }
    }
    return true;
}

OPENUPS_COLD static void config_print(const config_t* restrict config)
{
    if (config == NULL) {
        return;
    }
    printf("Configuration:\n");
    printf("  Target: %s\n", config->target);
    printf("  Interval: %d seconds\n", config->interval_sec);
    printf("  Threshold: %d\n", config->fail_threshold);
    printf("  Timeout: %d ms\n", config->timeout_ms);
    printf("  Payload Size: %d bytes\n", config->payload_size);
    printf("  Max Retries: %d\n", config->max_retries);
    printf("  IPv6: %s\n", config->use_ipv6 ? "true" : "false");
    printf("  Shutdown Mode: %s\n", shutdown_mode_to_string(config->shutdown_mode));
    printf("  Dry Run: %s\n", config->dry_run ? "true" : "false");
    printf("  Log Level: %s\n", log_level_to_string(config->log_level));
    printf("  Timestamp: %s\n", config->enable_timestamp ? "true" : "false");
    printf("  Systemd: %s\n", config->enable_systemd ? "true" : "false");
    printf("  Watchdog: %s\n", config->enable_watchdog ? "true" : "false");
}

OPENUPS_COLD static void config_print_usage(void)
{
    printf("Usage: %s [options]\n\n", OPENUPS_PROGRAM_NAME);
    printf("Network Options:\n");
    printf("  -t, --target <ip>           Target IP literal to monitor (DNS disabled, default: 1.1.1.1)\n");
    printf("  -i, --interval <sec>        Ping interval in seconds (default: 10)\n");
    printf("  -n, --threshold <num>       Consecutive failures threshold (default: 5)\n");
    printf("  -w, --timeout <ms>          Ping timeout in milliseconds (default: 2000)\n");
    printf("  -s, --payload-size <bytes>  ICMP payload size (default: 56)\n");
    printf("  -r, --retries <num>         Retry attempts per ping (default: 2)\n");
    printf("  -6, --ipv6[=true|false]     Enable/disable IPv6 mode (default: false)\n\n");
    printf("Shutdown Options:\n");
    printf("  -S, --shutdown-mode <mode>  Shutdown mode: immediate|delayed|log-only\n");
    printf("                              (default: immediate)\n");
    printf("  -D, --delay <min>           Shutdown delay in minutes for delayed mode (default: 1)\n");
    printf("  -d[ARG], --dry-run[=ARG]    Dry-run mode, no actual shutdown (default: true)\n");
    printf("                              ARG: true|false\n");
    printf("                              Note: Use -dfalse or --dry-run=false (no space)\n\n");
    printf("Logging Options:\n");
    printf("  -L, --log-level <level>     Log level: silent|error|warn|info|debug\n");
    printf("                              (default: info)\n");
    printf("  -T[ARG], --timestamp[=ARG]  Enable/disable log timestamps (default: true)\n");
    printf("                              ARG format: true|false\n\n");
    printf("System Integration:\n");
    printf("  -M[ARG], --systemd[=ARG]    Enable/disable systemd integration (default: true)\n");
    printf("  -W[ARG], --watchdog[=ARG]   Enable/disable systemd watchdog (default: true)\n");
    printf("                              ARG format: true|false\n\n");
    printf("General Options:\n");
    printf("  -v, --version               Show version information\n");
    printf("  -h, --help                  Show this help message\n\n");
    printf("Environment Variables (lower priority than CLI args):\n");
    printf("  Network:      OPENUPS_TARGET, OPENUPS_INTERVAL, OPENUPS_THRESHOLD,\n");
    printf("                OPENUPS_TIMEOUT, OPENUPS_PAYLOAD_SIZE, OPENUPS_RETRIES,\n");
    printf("                OPENUPS_IPV6\n");
    printf("  Shutdown:     OPENUPS_SHUTDOWN_MODE, OPENUPS_DELAY_MINUTES,\n");
    printf("                OPENUPS_DRY_RUN\n");
    printf("  Logging:      OPENUPS_LOG_LEVEL, OPENUPS_TIMESTAMP\n");
    printf("  Integration:  OPENUPS_SYSTEMD, OPENUPS_WATCHDOG\n");
    printf("\n");
    printf("Examples:\n");
    printf("  # Basic monitoring with dry-run\n");
    printf("  %s -t 1.1.1.1 -i 10 -n 5\n\n", OPENUPS_PROGRAM_NAME);
    printf("  # Production mode (actual shutdown)\n");
    printf("  %s -t 192.168.1.1 -i 5 -n 3 --dry-run=false\n\n", OPENUPS_PROGRAM_NAME);
    printf("  # Debug mode without timestamp (for systemd)\n");
    printf("  %s -t 8.8.8.8 -L debug --timestamp=false\n\n", OPENUPS_PROGRAM_NAME);
    printf("  # Short options (values must connect directly, no space)\n");
    printf("  %s -t 8.8.8.8 -i5 -n3 -dfalse -Tfalse -Ldebug\n\n", OPENUPS_PROGRAM_NAME);
}

OPENUPS_COLD static void config_print_version(void)
{
    printf("%s version %s\n", OPENUPS_PROGRAM_NAME, OPENUPS_VERSION);
    printf("OpenUPS network monitor\n");
}
/* ============================================================
 * ICMP functions
 * ============================================================ */

/* ICMP 校验和（IPv4 需要；IPv6 由内核处理）。 */
static uint16_t calculate_checksum_scalar(const void* data, size_t len)
{
    const uint16_t* buf = (const uint16_t*)data;
    uint32_t sum = 0;

    /* 累加 16-bit words */
    for (size_t i = 0; i < len / 2; i++) {
        sum += buf[i];
    }

    /* 奇数字节：按原字节值直接累加。 */
    if (len % 2) {
        sum += ((const uint8_t*)data)[len - 1];
    }

    /* 处理进位 */
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return ~sum;
}

#if OPENUPS_CAN_USE_CPU_SUPPORTS
__attribute__((target("avx2")))
static uint16_t calculate_checksum_avx2(const void* data, size_t len)
{
    const uint8_t* bytes = (const uint8_t*)data;
    uint32_t sum = 0;

    /* Process 32-byte blocks (16 x uint16_t). */
    size_t i = 0;

    const __m256i mask00ff = _mm256_set1_epi16(0x00FF);
    const __m256i ones = _mm256_set1_epi16(1);
    __m256i acc32 = _mm256_setzero_si256();

    size_t vec_len = len & ~(size_t)31;
    for (; i < vec_len; i += 32) {
        __m256i v = _mm256_loadu_si256((const __m256i*)(const void*)(bytes + i));

        /* Interpret as 16-bit little-endian words:
         * word = even_byte + (odd_byte << 8)
         */
        __m256i even16 = _mm256_and_si256(v, mask00ff);
        __m256i odd16 = _mm256_and_si256(_mm256_srli_epi16(v, 8), mask00ff);

        /* Pairwise sum into 32-bit lanes. */
        __m256i even32 = _mm256_madd_epi16(even16, ones);
        __m256i odd32 = _mm256_madd_epi16(odd16, ones);
        __m256i words32 = _mm256_add_epi32(even32, _mm256_slli_epi32(odd32, 8));

        acc32 = _mm256_add_epi32(acc32, words32);
    }

    /* Horizontal sum of 8 x u32 lanes. */
    __m128i lo = _mm256_castsi256_si128(acc32);
    __m128i hi = _mm256_extracti128_si256(acc32, 1);
    __m128i s = _mm_add_epi32(lo, hi);
    s = _mm_hadd_epi32(s, s);
    s = _mm_hadd_epi32(s, s);
    sum += (uint32_t)_mm_cvtsi128_si32(s);

    /* Tail: process remaining 16-bit words scalar. */
    size_t tail_start = vec_len;
    const uint16_t* tail16 = (const uint16_t*)(const void*)(bytes + tail_start);
    size_t tail_words = (len - tail_start) / 2;
    for (size_t w = 0; w < tail_words; w++) {
        sum += tail16[w];
    }

    /* Odd byte: added as-is (not shifted). */
    if (len % 2) {
        sum += bytes[len - 1];
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFFU) + (sum >> 16);
    }

    return (uint16_t)(~sum);
}
#endif

static uint16_t calculate_checksum(const void* data, size_t len)
{
#if OPENUPS_CAN_USE_CPU_SUPPORTS
    /* GCC/Clang on x86: safe runtime dispatch. */
    if (__builtin_cpu_supports("avx2")) {
        return calculate_checksum_avx2(data, len);
    }
#endif
    return calculate_checksum_scalar(data, len);
}

/* 初始化 ICMP pinger（需要 CAP_NET_RAW 权限） */
static bool icmp_pinger_init(icmp_pinger_t* restrict pinger, bool use_ipv6,
                             char* restrict error_msg, size_t error_size)
{
    if (pinger == NULL || error_msg == NULL || error_size == 0) {
        return false;
    }

    pinger->use_ipv6 = use_ipv6;
    pinger->sockfd = -1;
    pinger->sequence = 0;
    pinger->send_buf = NULL;
    pinger->send_buf_capacity = 0;
    pinger->payload_filled_size = 0;
    pinger->cached_target_valid = false;
    pinger->cached_target[0] = '\0';
    memset(&pinger->cached_addr, 0, sizeof(pinger->cached_addr));
    pinger->cached_addr_len = 0;

    /* 创建 raw socket */
    int family = use_ipv6 ? AF_INET6 : AF_INET;
    int protocol = use_ipv6 ? IPPROTO_ICMPV6 : IPPROTO_ICMP;

    int sock_type = SOCK_RAW;
#ifdef SOCK_CLOEXEC
    sock_type |= SOCK_CLOEXEC;
#endif
    pinger->sockfd = socket(family, sock_type, protocol);
    if (pinger->sockfd < 0) {
        snprintf(error_msg, error_size, "Failed to create socket: %s (require root or CAP_NET_RAW)",
                 strerror(errno));
        return false;
    }

    /* IPv6: checksum 由内核处理；尽力设置 IPV6_CHECKSUM（不支持时继续运行）。 */
    if (use_ipv6) {
        int offset = (int)offsetof(struct icmp6_hdr, icmp6_cksum);
        if (setsockopt(pinger->sockfd, IPPROTO_IPV6, IPV6_CHECKSUM, &offset,
                       (socklen_t)sizeof(offset)) != 0) {
            if (errno != EINVAL && errno != ENOPROTOOPT) {
                snprintf(error_msg, error_size, "Failed to set IPV6_CHECKSUM: %s",
                         strerror(errno));
                close(pinger->sockfd);
                pinger->sockfd = -1;
                return false;
            }
        }
    }

    /* 非阻塞：避免极端情况下 recvfrom 意外阻塞，配合 poll + EAGAIN 逻辑 */
    int flags = fcntl(pinger->sockfd, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(pinger->sockfd, F_SETFL, flags | O_NONBLOCK);
    }

    return true;
}

/* 销毁 ICMP pinger 并关闭 raw socket */
static void icmp_pinger_destroy(icmp_pinger_t* restrict pinger)
{
    if (pinger == NULL) {
        return;
    }

    if (pinger->sockfd >= 0) {
        close(pinger->sockfd);
        pinger->sockfd = -1;
    }

    free(pinger->send_buf);
    pinger->send_buf = NULL;
    pinger->send_buf_capacity = 0;
    pinger->payload_filled_size = 0;

    pinger->cached_target_valid = false;
    pinger->cached_target[0] = '\0';
    memset(&pinger->cached_addr, 0, sizeof(pinger->cached_addr));
    pinger->cached_addr_len = 0;
}

/* 解析目标地址 (IPv4 或 IPv6)
 * 约定: 仅支持 IP 字面量，不做 DNS 解析
 */
static bool resolve_target(const char* restrict target, bool use_ipv6,
                           struct sockaddr_storage* restrict addr, socklen_t* restrict addr_len,
                           char* restrict error_msg, size_t error_size)
{
    if (target == NULL || addr == NULL || addr_len == NULL || error_msg == NULL ||
        error_size == 0) {
        return false;
    }

    memset(addr, 0, sizeof(*addr));

    if (use_ipv6) {
        struct sockaddr_in6* addr6 = (struct sockaddr_in6*)addr;
        addr6->sin6_family = AF_INET6;
        if (inet_pton(AF_INET6, target, &addr6->sin6_addr) != 1) {
            snprintf(error_msg, error_size, "Invalid IPv6 address (DNS disabled): %s", target);
            return false;
        }
        *addr_len = sizeof(*addr6);
        return true;
    }

    struct sockaddr_in* addr4 = (struct sockaddr_in*)addr;
    addr4->sin_family = AF_INET;
    if (inet_pton(AF_INET, target, &addr4->sin_addr) != 1) {
        snprintf(error_msg, error_size, "Invalid IPv4 address (DNS disabled): %s", target);
        return false;
    }
    *addr_len = sizeof(*addr4);
    return true;
}

static bool ensure_send_buffer(icmp_pinger_t* restrict pinger, size_t need,
                               char* restrict error_msg, size_t error_size)
{
    if (pinger == NULL || error_msg == NULL || error_size == 0) {
        return false;
    }

    if (need <= pinger->send_buf_capacity && pinger->send_buf != NULL) {
        return true;
    }

    uint8_t* old_buf = pinger->send_buf;
    uint8_t* new_buf = (uint8_t*)realloc(pinger->send_buf, need);
    if (new_buf == NULL) {
        snprintf(error_msg, error_size, "Memory allocation failed");
        return false;
    }

    pinger->send_buf = new_buf;
    pinger->send_buf_capacity = need;
    if (new_buf != old_buf) {
        pinger->payload_filled_size = 0;
    }
    return true;
}

static void fill_payload_pattern(icmp_pinger_t* restrict pinger, size_t header_size,
                                 size_t payload_size)
{
    if (pinger == NULL || pinger->send_buf == NULL) {
        return;
    }

    if (payload_size == 0) {
        pinger->payload_filled_size = 0;
        return;
    }

    if (pinger->payload_filled_size == payload_size) {
        return;
    }

    for (size_t i = 0; i < payload_size; i++) {
        pinger->send_buf[header_size + i] = (uint8_t)(i & 0xFFU);
    }

    pinger->payload_filled_size = payload_size;
}

static uint16_t next_sequence(icmp_pinger_t* restrict pinger)
{
    if (pinger == NULL) {
        return 1;
    }

    /* 0 作为序列号可用但不常见；这里避免长期停留在 0。 */
    pinger->sequence = (uint16_t)(pinger->sequence + 1);
    if (pinger->sequence == 0) {
        pinger->sequence = 1;
    }
    return pinger->sequence;
}

static int wait_readable_with_tick(int fd, uint64_t deadline_ms, icmp_tick_fn tick,
                                   void* tick_user_data, icmp_should_stop_fn should_stop,
                                   void* stop_user_data)
{
    uint64_t last_tick_ms = 0;

    while (1) {
        if (should_stop != NULL && should_stop(stop_user_data)) {
            errno = EINTR;
            return -2;
        }

        uint64_t now_ms = get_monotonic_ms();
        if (now_ms == UINT64_MAX) {
            errno = EINVAL;
            return -1;
        }

        if (now_ms >= deadline_ms) {
            errno = EAGAIN;
            return -1;
        }

        /* 默认每 1 秒 tick 一次，避免过于频繁 */
        if (tick != NULL && (last_tick_ms == 0 || now_ms - last_tick_ms >= 1000)) {
            tick(tick_user_data);
            last_tick_ms = now_ms;
        }

        int timeout_ms = (int)(deadline_ms - now_ms);
        if (timeout_ms > 200) {
            timeout_ms = 200;
        }
        if (timeout_ms < 1) {
            timeout_ms = 1;
        }

        struct pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};

        int rc = poll(&pfd, 1, timeout_ms);
        if (rc > 0) {
            if ((pfd.revents & POLLIN) != 0) {
                return 0;
            }
            /* 其它事件（如错误）也让上层去 recvfrom 触发 errno */
            return 0;
        }
        if (rc == 0) {
            continue;
        }

        if (errno == EINTR) {
            continue;
        }
        return -1;
    }
}

/* 缓存 getpid() 结果（进程 PID 不会变化，避免重复系统调用） */
static uint16_t get_cached_ident(void)
{
    static uint16_t cached_ident = 0;
    if (OPENUPS_UNLIKELY(cached_ident == 0)) {
        cached_ident = (uint16_t)(getpid() & 0xFFFF);
        if (cached_ident == 0) {
            cached_ident = 1;
        }
    }
    return cached_ident;
}

/* IPv4 ping (ICMP Echo Request/Reply, RFC 792). */
static OPENUPS_HOT ping_result_t ping_ipv4(icmp_pinger_t* restrict pinger,
                               struct sockaddr_in* restrict dest_addr, int timeout_ms,
                               int payload_size, icmp_tick_fn tick, void* tick_user_data,
                               icmp_should_stop_fn should_stop, void* stop_user_data)
{
    ping_result_t result = {false, 0.0, ""};

    if (pinger == NULL || dest_addr == NULL) {
        snprintf(result.error_msg, sizeof(result.error_msg), "Invalid arguments");
        return result;
    }

    if (timeout_ms <= 0) {
        snprintf(result.error_msg, sizeof(result.error_msg), "Invalid timeout");
        return result;
    }

    /* 构建 ICMP Echo Request 数据包（复用缓冲区） */
    size_t packet_len = sizeof(struct icmphdr) + (size_t)payload_size;
    if (!ensure_send_buffer(pinger, packet_len, result.error_msg, sizeof(result.error_msg))) {
        return result;
    }
    fill_payload_pattern(pinger, sizeof(struct icmphdr), (size_t)payload_size);

    struct icmphdr* icmp_hdr = (struct icmphdr*)pinger->send_buf;
    memset(icmp_hdr, 0, sizeof(*icmp_hdr));
    icmp_hdr->type = ICMP_ECHO;
    icmp_hdr->code = 0;
    uint16_t ident = get_cached_ident();
    uint16_t seq = next_sequence(pinger);
    icmp_hdr->un.echo.id = ident;
    icmp_hdr->un.echo.sequence = seq;

    /* 计算校验和：先清零，再计算。 */
    icmp_hdr->checksum = 0;
    icmp_hdr->checksum = calculate_checksum(pinger->send_buf, packet_len);

    /* 发送 ICMP Echo Request */
    struct timespec send_time;
    (void)clock_gettime(CLOCK_MONOTONIC, &send_time);

    ssize_t sent = sendto(pinger->sockfd, pinger->send_buf, packet_len, MSG_NOSIGNAL,
                          (struct sockaddr*)dest_addr,
                          sizeof(*dest_addr));
    if (OPENUPS_UNLIKELY(sent < 0)) {
        snprintf(result.error_msg, sizeof(result.error_msg), "Failed to send packet: %s",
                 strerror(errno));
        return result;
    }

    /* 接收 ICMP Echo Reply (包含 IP 头和 ICMP 头) */
    uint8_t recv_buf[4096]
        __attribute__((aligned(16)));  /* 对齐优化，改善结构体访问性能 */
    struct sockaddr_in recv_addr;
    socklen_t recv_addr_len = sizeof(recv_addr);

    uint64_t send_ms = (uint64_t)send_time.tv_sec * 1000ULL +
                       (uint64_t)send_time.tv_nsec / 1000000ULL;
    uint64_t deadline_ms = send_ms + (uint64_t)timeout_ms;

    while (1) {
        int wait_rc = wait_readable_with_tick(pinger->sockfd, deadline_ms, tick, tick_user_data,
                                              should_stop, stop_user_data);
        if (wait_rc == -2) {
            snprintf(result.error_msg, sizeof(result.error_msg), "Stopped");
            return result;
        }
        if (wait_rc < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                snprintf(result.error_msg, sizeof(result.error_msg), "Timeout");
            } else {
                snprintf(result.error_msg, sizeof(result.error_msg), "Failed to wait: %s",
                         strerror(errno));
            }
            return result;
        }

        ssize_t received;
        recv_addr_len = sizeof(recv_addr);
        do {
            received = recvfrom(pinger->sockfd, recv_buf, sizeof(recv_buf), 0,
                                (struct sockaddr*)&recv_addr, &recv_addr_len);
        } while (received < 0 && errno == EINTR);

        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            snprintf(result.error_msg, sizeof(result.error_msg), "Failed to receive: %s",
                     strerror(errno));
            return result;
        }

        /* 快速过滤：只接受来自目标的回包 */
        if (recv_addr.sin_addr.s_addr != dest_addr->sin_addr.s_addr) {
            continue;
        }

        /* 解析 IP 头和 ICMP 头 (IP 头长度可变) */
        if (received < (ssize_t)sizeof(struct ip)) {
            continue;
        }

        struct ip* ip_hdr = (struct ip*)recv_buf;
        if (ip_hdr->ip_p != IPPROTO_ICMP) {
            continue;
        }

        size_t ip_hdr_len = (size_t)ip_hdr->ip_hl * 4;
        if (ip_hdr_len < sizeof(struct ip) || ip_hdr_len > (size_t)received) {
            continue;
        }

        if (ip_hdr_len + sizeof(struct icmphdr) > (size_t)received) {
            continue;
        }

        struct icmphdr* recv_icmp = (struct icmphdr*)(recv_buf + ip_hdr_len);
        if (recv_icmp->type != ICMP_ECHOREPLY) {
            continue;
        }
        if (recv_icmp->un.echo.id != ident || recv_icmp->un.echo.sequence != seq) {
            continue;
        }

        struct timespec recv_time;
        (void)clock_gettime(CLOCK_MONOTONIC, &recv_time);

        double latency = (recv_time.tv_sec - send_time.tv_sec) * 1000.0 +
                         (recv_time.tv_nsec - send_time.tv_nsec) / 1000000.0;
        if (latency < 0.0) {
            latency = 0.0;
        }

        result.success = true;
        result.latency_ms = latency;
        return result;
    }
}

/* IPv6 ping (ICMPv6 Echo Request/Reply, RFC 4443). */
static OPENUPS_HOT ping_result_t ping_ipv6(icmp_pinger_t* restrict pinger,
                               struct sockaddr_in6* restrict dest_addr, int timeout_ms,
                               int payload_size, icmp_tick_fn tick, void* tick_user_data,
                               icmp_should_stop_fn should_stop, void* stop_user_data)
{
    ping_result_t result = {false, 0.0, ""};

    if (pinger == NULL || dest_addr == NULL) {
        snprintf(result.error_msg, sizeof(result.error_msg), "Invalid arguments");
        return result;
    }

    if (timeout_ms <= 0) {
        snprintf(result.error_msg, sizeof(result.error_msg), "Invalid timeout");
        return result;
    }

    /* 构造 ICMPv6 Echo Request 数据包（复用缓冲区） */
    size_t packet_len = sizeof(struct icmp6_hdr) + (size_t)payload_size;
    if (!ensure_send_buffer(pinger, packet_len, result.error_msg, sizeof(result.error_msg))) {
        return result;
    }
    fill_payload_pattern(pinger, sizeof(struct icmp6_hdr), (size_t)payload_size);

    struct icmp6_hdr* icmp6_hdr = (struct icmp6_hdr*)pinger->send_buf;
    memset(icmp6_hdr, 0, sizeof(*icmp6_hdr));
    icmp6_hdr->icmp6_type = ICMP6_ECHO_REQUEST; /* 类型: Echo Request = 128 */
    icmp6_hdr->icmp6_code = 0;                  /* 代码: 0 */
    uint16_t ident = get_cached_ident();
    uint16_t seq = next_sequence(pinger);
    icmp6_hdr->icmp6_id = ident;
    icmp6_hdr->icmp6_seq = seq;

    /* IPv6 checksum 由内核处理（细节见 TECHNICAL.md）。 */

    /* 发送 ICMPv6 Echo Request */
    struct timespec send_time;
    (void)clock_gettime(CLOCK_MONOTONIC, &send_time);

    ssize_t sent = sendto(pinger->sockfd, pinger->send_buf, packet_len, MSG_NOSIGNAL,
                          (struct sockaddr*)dest_addr,
                          sizeof(*dest_addr));
    if (OPENUPS_UNLIKELY(sent < 0)) {
        snprintf(result.error_msg, sizeof(result.error_msg), "Failed to send packet: %s",
                 strerror(errno));
        return result;
    }

    /* 接收 ICMPv6 Echo Reply */
    uint8_t recv_buf[4096]
        __attribute__((aligned(16)));  /* 对齐优化 */
    struct sockaddr_in6 recv_addr;
    socklen_t recv_addr_len = sizeof(recv_addr);

    uint64_t send_ms = (uint64_t)send_time.tv_sec * 1000ULL +
                       (uint64_t)send_time.tv_nsec / 1000000ULL;
    uint64_t deadline_ms = send_ms + (uint64_t)timeout_ms;

    while (1) {
        int wait_rc = wait_readable_with_tick(pinger->sockfd, deadline_ms, tick, tick_user_data,
                                              should_stop, stop_user_data);
        if (wait_rc == -2) {
            snprintf(result.error_msg, sizeof(result.error_msg), "Stopped");
            return result;
        }
        if (wait_rc < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                snprintf(result.error_msg, sizeof(result.error_msg), "Timeout");
            } else {
                snprintf(result.error_msg, sizeof(result.error_msg), "Failed to wait: %s",
                         strerror(errno));
            }
            return result;
        }

        ssize_t received;
        recv_addr_len = sizeof(recv_addr);
        do {
            received = recvfrom(pinger->sockfd, recv_buf, sizeof(recv_buf), 0,
                                (struct sockaddr*)&recv_addr, &recv_addr_len);
        } while (received < 0 && errno == EINTR);

        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            snprintf(result.error_msg, sizeof(result.error_msg), "Failed to receive: %s",
                     strerror(errno));
            return result;
        }

        /* 过滤源地址 */
        if (memcmp(&recv_addr.sin6_addr, &dest_addr->sin6_addr, sizeof(struct in6_addr)) != 0) {
            continue;
        }

        if (received < (ssize_t)sizeof(struct icmp6_hdr)) {
            continue;
        }

        struct icmp6_hdr* recv_icmp6 = (struct icmp6_hdr*)recv_buf;
        if (recv_icmp6->icmp6_type != ICMP6_ECHO_REPLY) {
            continue;
        }
        if (recv_icmp6->icmp6_id != ident || recv_icmp6->icmp6_seq != seq) {
            continue;
        }

        struct timespec recv_time;
        (void)clock_gettime(CLOCK_MONOTONIC, &recv_time);

        double latency = (recv_time.tv_sec - send_time.tv_sec) * 1000.0 +
                         (recv_time.tv_nsec - send_time.tv_nsec) / 1000000.0;
        if (latency < 0.0) {
            latency = 0.0;
        }

        result.success = true;
        result.latency_ms = latency;
        return result;
    }
}

/* Forward declaration for icmp_pinger_ping_ex */
static OPENUPS_HOT ping_result_t icmp_pinger_ping_ex(icmp_pinger_t* restrict pinger,
                                                const char* restrict target, int timeout_ms,
                                                int payload_size, icmp_tick_fn tick,
                                                void* tick_user_data,
                                                icmp_should_stop_fn should_stop,
                                                void* stop_user_data);

/* 执行 ping 操作 (自动根据地址类型选择 IPv4/IPv6） */
__attribute__((unused))
static OPENUPS_HOT ping_result_t icmp_pinger_ping(icmp_pinger_t* restrict pinger,
                               const char* restrict target,
                               int timeout_ms, int payload_size)
{
    return icmp_pinger_ping_ex(pinger, target, timeout_ms, payload_size, NULL, NULL, NULL, NULL);
}

static OPENUPS_HOT ping_result_t icmp_pinger_ping_ex(icmp_pinger_t* restrict pinger,
                                  const char* restrict target,
                                  int timeout_ms, int payload_size, icmp_tick_fn tick,
                                  void* tick_user_data, icmp_should_stop_fn should_stop,
                                  void* stop_user_data)
{
    ping_result_t result = {false, 0.0, ""};

    if (pinger == NULL || target == NULL) {
        snprintf(result.error_msg, sizeof(result.error_msg), "Invalid arguments");
        return result;
    }

    if (timeout_ms <= 0) {
        snprintf(result.error_msg, sizeof(result.error_msg), "Invalid timeout");
        return result;
    }

    if (payload_size < 0) {
        snprintf(result.error_msg, sizeof(result.error_msg), "Invalid payload size");
        return result;
    }

    const int max_payload = pinger->use_ipv6 ? 65487 : 65507;
    if (payload_size > max_payload) {
        snprintf(result.error_msg, sizeof(result.error_msg),
                 "Payload size must be between 0 and %d", max_payload);
        return result;
    }

    /* 解析目标地址（带缓存） */
    const struct sockaddr_storage* addr_ptr = NULL;
    socklen_t addr_len = 0;

    if (pinger->cached_target_valid &&
        strncmp(pinger->cached_target, target, sizeof(pinger->cached_target)) == 0) {
        addr_ptr = &pinger->cached_addr;
        addr_len = pinger->cached_addr_len;
    } else {
        struct sockaddr_storage addr;
        if (!resolve_target(target, pinger->use_ipv6, &addr, &addr_len, result.error_msg,
                            sizeof(result.error_msg))) {
            return result;
        }

        /* 尽量缓存（target 来自 config/CLI，正常长度 < 256） */
        (void)snprintf(pinger->cached_target, sizeof(pinger->cached_target), "%s", target);
        pinger->cached_addr = addr;
        pinger->cached_addr_len = addr_len;
        pinger->cached_target_valid = true;
        addr_ptr = &pinger->cached_addr;
    }

    (void)addr_len;
    if (pinger->use_ipv6) {
        return ping_ipv6(pinger, (struct sockaddr_in6*)(void*)addr_ptr, timeout_ms, payload_size,
                         tick, tick_user_data, should_stop, stop_user_data);
    }
    return ping_ipv4(pinger, (struct sockaddr_in*)(void*)addr_ptr, timeout_ms, payload_size, tick,
                     tick_user_data, should_stop, stop_user_data);
}

/* ============================================================
 * Shutdown functions
 * ============================================================ */



/* ============================================================
 * shutdown 关机触发
 * ============================================================ */

/* 构建关机命令参数数组（空白分隔，不支持引号） */
static bool build_shutdown_argv(const char* command, char* buffer, size_t buffer_size,
                                char* argv[], size_t argv_size)
{
    if (command == NULL || buffer == NULL || argv == NULL || argv_size < 2) {
        return false;
    }

    size_t cmd_len = strlen(command);
    if (cmd_len == 0 || cmd_len >= buffer_size) {
        return false;
    }

    /* 严格拒绝引号/反引号与控制字符。 */
    if (strchr(command, '"') != NULL || strchr(command, '\'') != NULL ||
        strchr(command, '`') != NULL) {
        return false;
    }

    for (const char* p = command; *p != '\0'; ++p) {
        if ((unsigned char)*p < 0x20 || *p == 0x7F) {
            return false;
        }
    }

    memcpy(buffer, command, cmd_len + 1);

    size_t argc = 0;
    char* saveptr = NULL;
    char* token = strtok_r(buffer, " \t", &saveptr);
    while (token != NULL) {
        if (argc + 1 >= argv_size) {
            return false;
        }
        if (!is_safe_path(token)) {
            return false;
        }
        argv[argc++] = token;
        token = strtok_r(NULL, " \t", &saveptr);
    }

    if (argc == 0) {
        return false;
    }

    argv[argc] = NULL;
    return true;
}

static bool shutdown_select_command(const config_t* restrict config, bool use_systemctl_poweroff,
                                    char* restrict command_buf, size_t command_size)
{
    if (config == NULL || command_buf == NULL || command_size == 0) {
        return false;
    }

    if (config->shutdown_mode == SHUTDOWN_MODE_IMMEDIATE) {
        if (use_systemctl_poweroff) {
            snprintf(command_buf, command_size, "systemctl poweroff");
        } else {
            snprintf(command_buf, command_size, "/sbin/shutdown -h now");
        }
        return true;
    }

    if (config->shutdown_mode == SHUTDOWN_MODE_DELAYED) {
        snprintf(command_buf, command_size, "/sbin/shutdown -h +%d", config->delay_minutes);
        return true;
    }

    return false;
}

static void log_shutdown_plan(logger_t* restrict logger, shutdown_mode_t mode,
                              int delay_minutes)
{
    if (logger == NULL) {
        return;
    }

    if (mode == SHUTDOWN_MODE_IMMEDIATE) {
        logger_warn(logger, "Triggering immediate shutdown");
    } else if (mode == SHUTDOWN_MODE_DELAYED) {
        logger_warn(logger, "Triggering shutdown in %d minutes", delay_minutes);
    }
}

static OPENUPS_COLD bool shutdown_should_execute(const config_t* restrict config, logger_t* restrict logger)
{
    if (config == NULL || logger == NULL) {
        return false;
    }

    if (config->dry_run) {
        logger_info(logger, "[DRY-RUN] Would trigger shutdown in %s mode",
                    shutdown_mode_to_string(config->shutdown_mode));
        return false;
    }

    if (config->shutdown_mode == SHUTDOWN_MODE_LOG_ONLY) {
        logger_error(logger, "LOG-ONLY mode: Network connectivity lost, would trigger shutdown");
        return false;
    }

    return true;
}

static OPENUPS_COLD bool shutdown_execute_command(char* argv[], logger_t* restrict logger)
{
    if (argv == NULL || argv[0] == NULL || logger == NULL) {
        return false;
    }

    /* 使用 fork/execvp 执行（不经过 shell）。 */
    pid_t child_pid = fork();
    if (child_pid < 0) {
        logger_error(logger, "fork() failed: %s", strerror(errno));
        return false;
    }

    if (child_pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            (void)dup2(devnull, STDOUT_FILENO);
            close(devnull);
        }

        execvp(argv[0], argv);

        fprintf(stderr, "exec failed: %s\n", strerror(errno));
        _exit(127);
    }

    /* 父进程：等待子进程完成（5 秒超时）。 */
    int status = 0;
    uint64_t start_ms = get_monotonic_ms();
    bool use_monotonic = start_ms != UINT64_MAX;
    if (!use_monotonic) {
        start_ms = (uint64_t)time(NULL) * UINT64_C(1000);
    }
    while (1) {
        pid_t result = waitpid(child_pid, &status, WNOHANG);
        if (result == child_pid) {
            if (WIFEXITED(status)) {
                int exit_code = WEXITSTATUS(status);
                if (exit_code == 0) {
                    logger_info(logger, "Shutdown command executed successfully");
                } else {
                    logger_error(logger, "Shutdown command failed with exit code %d: %s",
                                 exit_code, argv[0]);
                }
            } else if (WIFSIGNALED(status)) {
                logger_error(logger, "Shutdown command terminated by signal %d: %s",
                             WTERMSIG(status), argv[0]);
            }
            return true;
        }

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            logger_error(logger, "waitpid() failed: %s", strerror(errno));
            return false;
        }

        uint64_t now_ms = use_monotonic ? get_monotonic_ms()
                                        : (uint64_t)time(NULL) * UINT64_C(1000);
        if (now_ms == UINT64_MAX) {
            now_ms = (uint64_t)time(NULL) * UINT64_C(1000);
            use_monotonic = false;
        }

        if (now_ms - start_ms > UINT64_C(5000)) {
            logger_warn(logger, "Shutdown command timeout, killing process");
            kill(child_pid, SIGKILL);
            (void)waitpid(child_pid, &status, 0);
            return false;
        }

        usleep(100000);
    }
}

static OPENUPS_COLD void shutdown_trigger(const config_t* config, logger_t* logger, bool use_systemctl_poweroff)
{
    if (config == NULL || logger == NULL) {
        return;
    }

    logger_warn(logger, "Shutdown threshold reached, mode is %s%s",
                shutdown_mode_to_string(config->shutdown_mode),
                config->dry_run ? " (dry-run enabled)" : "");

    if (!shutdown_should_execute(config, logger)) {
        return;
    }

    char command_buf[512];
    char* argv[16] = {0};

    if (!shutdown_select_command(config, use_systemctl_poweroff, command_buf,
                                 sizeof(command_buf))) {
        logger_error(logger, "Unknown shutdown mode");
        return;
    }

    if (!build_shutdown_argv(command_buf, command_buf, sizeof(command_buf), argv,
                             sizeof(argv) / sizeof(argv[0]))) {
        logger_error(logger, "Failed to parse shutdown command: %s", command_buf);
        return;
    }

    log_shutdown_plan(logger, config->shutdown_mode, config->delay_minutes);

    (void)shutdown_execute_command(argv, logger);
}

/* ============================================================
 * Systemd functions
 * ============================================================ */



/* ============================================================
 * systemd 通知器
 * ============================================================ */

static bool build_systemd_addr(const char* restrict socket_path, struct sockaddr_un* restrict addr,
                              socklen_t* restrict addr_len)
{
    if (socket_path == NULL || addr == NULL || addr_len == NULL) {
        return false;
    }

    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;

    /* 处理抽象命名空间（@ 前缀） */
    if (socket_path[0] == '@') {
        size_t name_len = strlen(socket_path + 1);
        if (name_len == 0 || name_len > sizeof(addr->sun_path) - 1) {
            return false;
        }

        addr->sun_path[0] = '\0';
        memcpy(addr->sun_path + 1, socket_path + 1, name_len);
        *addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + name_len);
        return true;
    }

    size_t path_len = strlen(socket_path);
    if (path_len == 0 || path_len >= sizeof(addr->sun_path)) {
        return false;
    }

    memcpy(addr->sun_path, socket_path, path_len + 1);
    *addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_len + 1);
    return true;
}

/* 发送 systemd 通知消息（READY/STATUS/STOPPING/WATCHDOG）。 */
static OPENUPS_HOT bool send_notify(systemd_notifier_t* restrict notifier, const char* restrict message)
{
    if (OPENUPS_UNLIKELY(notifier == NULL || message == NULL || !notifier->enabled)) {
        return false;
    }

    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif

    /* 重试发送直到成功或遇到非 EINTR 错误 */
    ssize_t sent;
    do {
        sent = send(notifier->sockfd, message, strlen(message), flags);
    } while (sent < 0 && errno == EINTR);

    return sent >= 0;
}

static void systemd_notifier_init(systemd_notifier_t* restrict notifier)
{
    if (notifier == NULL) {
        return;
    }

    notifier->enabled = false;
    notifier->sockfd = -1;
    notifier->watchdog_usec = 0;
    memset(&notifier->addr, 0, sizeof(notifier->addr));
    notifier->addr_len = 0;
    notifier->last_watchdog_ms = 0;
    notifier->last_status_ms = 0;
    notifier->last_status[0] = '\0';

    /* 非 systemd 管理时通常不会设置 NOTIFY_SOCKET。 */
    const char* socket_path = getenv("NOTIFY_SOCKET");
    if (socket_path == NULL) {
        return;
    }

    /* systemd 通知使用 AF_UNIX/SOCK_DGRAM。 */
    int socket_type = SOCK_DGRAM;
#ifdef SOCK_CLOEXEC
    socket_type |= SOCK_CLOEXEC;
#endif
    notifier->sockfd = socket(AF_UNIX, socket_type, 0);
    if (notifier->sockfd < 0) {
        return;
    }

    if (!build_systemd_addr(socket_path, &notifier->addr, &notifier->addr_len)) {
        close(notifier->sockfd);
        notifier->sockfd = -1;
        return;
    }

    int rc;
    do {
        rc = connect(notifier->sockfd, (const struct sockaddr*)&notifier->addr, notifier->addr_len);
    } while (rc < 0 && errno == EINTR);
    if (rc < 0) {
        close(notifier->sockfd);
        notifier->sockfd = -1;
        return;
    }

    notifier->enabled = true;

    /* watchdog 超时时间（微秒）；用于计算心跳间隔。 */
    const char* watchdog_str = getenv("WATCHDOG_USEC");
    if (watchdog_str != NULL) {
        notifier->watchdog_usec = strtoull(watchdog_str, NULL, 10);
    }
}

static void systemd_notifier_destroy(systemd_notifier_t* restrict notifier)
{
    if (notifier == NULL) {
        return;
    }

    if (notifier->sockfd >= 0) {
        close(notifier->sockfd);
        notifier->sockfd = -1;
    }

    notifier->enabled = false;
    memset(&notifier->addr, 0, sizeof(notifier->addr));
    notifier->addr_len = 0;
}

/* 检查 systemd 通知器是否已启用 */
static OPENUPS_PURE bool systemd_notifier_is_enabled(const systemd_notifier_t* restrict notifier)
{
    return notifier != NULL && notifier->enabled;
}

/* 通知 systemd 程序已就绪 (启动完成) */
static bool systemd_notifier_ready(systemd_notifier_t* restrict notifier)
{
    return send_notify(notifier, "READY=1");
}

/* 更新程序状态信息到 systemd */
static OPENUPS_HOT bool systemd_notifier_status(systemd_notifier_t* restrict notifier, const char* restrict status)
{
    if (notifier == NULL || !notifier->enabled || status == NULL) {
        return false;
    }

    /* 降频：内容未变化且距离上次发送太近时跳过 */
    uint64_t now_ms = get_monotonic_ms();
    bool same = (strncmp(notifier->last_status, status, sizeof(notifier->last_status)) == 0);
    if (same && notifier->last_status_ms != 0 && now_ms - notifier->last_status_ms < 2000) {
        return true;
    }

    /* systemd 约定：STATUS=... */
    char message[256];
    snprintf(message, sizeof(message), "STATUS=%s", status);

    bool ok = send_notify(notifier, message);
    if (ok) {
        snprintf(notifier->last_status, sizeof(notifier->last_status), "%s", status);
        notifier->last_status_ms = now_ms;
    }
    return ok;
}

/* 通知 systemd 程序即将停止运行 */
static bool systemd_notifier_stopping(systemd_notifier_t* restrict notifier)
{
    return send_notify(notifier, "STOPPING=1");
}

static OPENUPS_HOT bool systemd_notifier_watchdog(systemd_notifier_t* restrict notifier)
{
    if (OPENUPS_UNLIKELY(notifier == NULL || !notifier->enabled)) {
        return false;
    }

    /* 未设置 WatchdogSec 时 systemd 不会提供 WATCHDOG_USEC，此时应默认 no-op。 */
    if (OPENUPS_UNLIKELY(notifier->watchdog_usec == 0)) {
        return true;
    }

    uint64_t now_ms = get_monotonic_ms();
    uint64_t interval_ms = notifier->watchdog_usec / 2000ULL; /* usec/2 -> ms */
    if (interval_ms == 0) {
        interval_ms = 1;
    }

    if (notifier->last_watchdog_ms != 0 && now_ms - notifier->last_watchdog_ms < interval_ms) {
        return true;
    }

    bool ok = send_notify(notifier, "WATCHDOG=1");
    if (ok) {
        notifier->last_watchdog_ms = now_ms;
    }
    return ok;
}

static OPENUPS_PURE uint64_t systemd_notifier_watchdog_interval_ms(const systemd_notifier_t* restrict notifier)
{
    if (notifier == NULL || !notifier->enabled || notifier->watchdog_usec == 0) {
        return 0;
    }

    uint64_t interval_ms = notifier->watchdog_usec / 2000ULL; /* usec/2 -> ms */
    if (interval_ms == 0) {
        interval_ms = 1;
    }
    return interval_ms;
}

/* ============================================================
 * Context functions
 * ============================================================ */


/* === 全局上下文指针（仅用于信号处理） === */
static openups_ctx_t* g_ctx = NULL;

/* === 信号处理 === */

static void signal_handler(int signum)
{
    if (g_ctx == NULL) {
        return;
    }

    switch (signum) {
    case SIGINT:
    case SIGTERM:
        g_ctx->stop_flag = 1;
        break;
    case SIGUSR1:
        g_ctx->print_stats_flag = 1;
        break;
    default:
        break;
    }
}

static void setup_signal_handlers(openups_ctx_t* restrict ctx)
{
    if (ctx == NULL) {
        return;
    }

    g_ctx = ctx;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);

    (void)sigaction(SIGINT, &sa, NULL);
    (void)sigaction(SIGTERM, &sa, NULL);
    (void)sigaction(SIGUSR1, &sa, NULL);
}

/* === 内部辅助函数 === */

/* watchdog 心跳回调（在 ICMP 等待期间定期发送） */
static void watchdog_tick_callback(void* user_data)
{
    openups_ctx_t* ctx = (openups_ctx_t*)user_data;
    if (ctx == NULL || !ctx->config.enable_watchdog || !ctx->systemd_enabled) {
        return;
    }

    (void)systemd_notifier_watchdog(&ctx->systemd);
}

/* 监控停止条件回调（ICMP 等待期间检查是否应中断） */
static bool monitor_should_stop(void* user_data)
{
    openups_ctx_t* ctx = (openups_ctx_t*)user_data;
    return ctx != NULL && ctx->stop_flag != 0;
}

/* systemd 状态通知辅助函数 */
static void notify_systemd_status(openups_ctx_t* restrict ctx, const char* restrict fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void notify_systemd_status(openups_ctx_t* restrict ctx, const char* restrict fmt, ...)
{
    if (ctx == NULL || !ctx->systemd_enabled || fmt == NULL) {
        return;
    }

    char status_msg[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(status_msg, sizeof(status_msg), fmt, args);
    va_end(args);

    (void)systemd_notifier_status(&ctx->systemd, status_msg);
}

/* === 上下文初始化 === */

static bool openups_ctx_init(openups_ctx_t* restrict ctx, int argc, char** restrict argv,
                      char* restrict error_msg, size_t error_size)
{
    if (ctx == NULL || error_msg == NULL || error_size == 0) {
        return false;
    }

    /* 零初始化 */
    memset(ctx, 0, sizeof(*ctx));
    ctx->start_time_ms = get_monotonic_ms();

    /* === 配置加载（3 层优先级） === */
    config_init_default(&ctx->config);
    config_load_from_env(&ctx->config);

    if (!config_load_from_cmdline(&ctx->config, argc, argv)) {
        snprintf(error_msg, error_size, "Failed to parse command line arguments");
        return false;
    }

    if (!config_validate(&ctx->config, error_msg, error_size)) {
        return false;
    }

    /* === 日志器初始化 === */
    logger_init(&ctx->logger, ctx->config.log_level, ctx->config.enable_timestamp);

    /* 调试模式下打印配置 */
    if (ctx->config.log_level == LOG_LEVEL_DEBUG) {
        config_print(&ctx->config);
    }

    /* === ICMP pinger 初始化 === */
    if (!icmp_pinger_init(&ctx->pinger, ctx->config.use_ipv6, error_msg, error_size)) {
        logger_destroy(&ctx->logger);
        return false;
    }

    /* === 指标初始化 === */
    metrics_init(&ctx->metrics);

    /* === systemd 集成初始化 === */
    if (ctx->config.enable_systemd) {
        systemd_notifier_init(&ctx->systemd);
        ctx->systemd_enabled = systemd_notifier_is_enabled(&ctx->systemd);

        if (ctx->systemd_enabled) {
            logger_debug(&ctx->logger, "systemd integration enabled");

            if (ctx->config.enable_watchdog) {
                ctx->watchdog_interval_ms = systemd_notifier_watchdog_interval_ms(&ctx->systemd);
                logger_debug(&ctx->logger, "watchdog interval: %" PRIu64 "ms",
                             ctx->watchdog_interval_ms);
            }
        } else {
            logger_debug(&ctx->logger, "systemd not detected, integration disabled");
        }
    } else {
        systemd_notifier_destroy(&ctx->systemd);
    }

    return true;
}

static void openups_ctx_destroy(openups_ctx_t* restrict ctx)
{
    if (ctx == NULL) {
        return;
    }

    icmp_pinger_destroy(&ctx->pinger);
    systemd_notifier_destroy(&ctx->systemd);
    logger_destroy(&ctx->logger);

    g_ctx = NULL;
    memset(ctx, 0, sizeof(*ctx));
}

/* === 统计信息打印 === */

static OPENUPS_COLD void openups_ctx_print_stats(openups_ctx_t* restrict ctx)
{
    if (ctx == NULL) {
        return;
    }

    const metrics_t* metrics = &ctx->metrics;

    logger_info(&ctx->logger,
                "Statistics: %" PRIu64 " total pings, %" PRIu64 " successful, %" PRIu64
                " failed (%.2f%% success rate), latency min %.2fms / max %.2fms / avg %.2fms, "
                "uptime %" PRIu64 " seconds",
                metrics->total_pings, metrics->successful_pings, metrics->failed_pings,
                metrics_success_rate(metrics), metrics->min_latency, metrics->max_latency,
                metrics_avg_latency(metrics), metrics_uptime_seconds(metrics));
}

/* === Ping 执行（带重试） === */

static OPENUPS_HOT bool openups_ctx_ping_once(openups_ctx_t* restrict ctx, ping_result_t* restrict result)
{
    if (ctx == NULL || result == NULL) {
        return false;
    }

    const config_t* config = &ctx->config;

    /* 执行 ping（带重试机制） */
    for (int attempt = 0; attempt <= config->max_retries; attempt++) {
        ctx->last_ping_time_ms = get_monotonic_ms();

        *result = icmp_pinger_ping_ex(&ctx->pinger, config->target, config->timeout_ms,
                                      config->payload_size, watchdog_tick_callback, ctx,
                                      monitor_should_stop, ctx);

        if (result->success) {
            return true;
        }

        if (ctx->stop_flag) {
            return false;
        }

        /* 重试前延迟 100ms */
        if (attempt < config->max_retries) {
            struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000L};
            while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
                if (ctx->stop_flag) {
                    break;
                }
            }
        }
    }

    return false;
}

/* === 可中断休眠 === */

static OPENUPS_HOT void openups_ctx_sleep_interruptible(openups_ctx_t* restrict ctx, int seconds)
{
    if (ctx == NULL || seconds <= 0) {
        return;
    }

    const bool watchdog_enabled = ctx->systemd_enabled && ctx->config.enable_watchdog;
    const uint64_t watchdog_interval_ms = watchdog_enabled ? ctx->watchdog_interval_ms : 0;

    uint64_t remaining_ms = 0;
    if (ckd_mul(&remaining_ms, (uint64_t)seconds, UINT64_C(1000))) {
        remaining_ms = UINT64_MAX;
    }

    while (remaining_ms > 0) {
        if (OPENUPS_UNLIKELY(ctx->stop_flag)) {
            break;
        }

        /* 计算本次休眠时长（不超过 watchdog 间隔） */
        uint64_t chunk_ms = remaining_ms;
        if (watchdog_enabled && watchdog_interval_ms > 0 && watchdog_interval_ms < chunk_ms) {
            chunk_ms = watchdog_interval_ms;
        }

        /* 休眠 */
        struct timespec ts = {.tv_sec = (time_t)(chunk_ms / 1000ULL),
                              .tv_nsec = (long)((chunk_ms % 1000ULL) * 1000000ULL)};
        while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
            if (ctx->stop_flag) {
                break;
            }
        }

        /* 喂 watchdog */
        if (watchdog_enabled) {
            (void)systemd_notifier_watchdog(&ctx->systemd);
        }

        /* 更新剩余时间 */
        if (chunk_ms >= remaining_ms) {
            remaining_ms = 0;
        } else {
            remaining_ms -= chunk_ms;
        }
    }
}

/* === 监控循环核心逻辑 === */

/* 处理 ping 成功 */
static OPENUPS_HOT void handle_ping_success(openups_ctx_t* restrict ctx, const ping_result_t* restrict result)
{
    if (ctx == NULL || result == NULL) {
        return;
    }

    ctx->consecutive_fails = 0;
    metrics_record_success(&ctx->metrics, result->latency_ms);

    logger_debug(&ctx->logger, "Ping successful to %s, latency: %.2fms",
                 ctx->config.target, result->latency_ms);

    /* systemd 状态通知 */
    if (ctx->systemd_enabled) {
        const metrics_t* metrics = &ctx->metrics;
        double success_rate = metrics_success_rate(metrics);
        notify_systemd_status(ctx,
                              "OK: %" PRIu64 "/%" PRIu64 " pings (%.1f%%), latency %.2fms",
                              metrics->successful_pings, metrics->total_pings, success_rate,
                              result->latency_ms);
    }
}

/* 处理 ping 失败 */
static OPENUPS_COLD void handle_ping_failure(openups_ctx_t* restrict ctx, const ping_result_t* restrict result)
{
    if (ctx == NULL || result == NULL) {
        return;
    }

    ctx->consecutive_fails++;
    metrics_record_failure(&ctx->metrics);

    logger_warn(&ctx->logger, "Ping failed to %s: %s (consecutive failures: %d)",
                ctx->config.target, result->error_msg, ctx->consecutive_fails);

    /* systemd 状态通知 */
    if (ctx->systemd_enabled) {
        notify_systemd_status(ctx, "WARNING: %d consecutive failures, threshold is %d",
                              ctx->consecutive_fails, ctx->config.fail_threshold);
    }
}

/* 触发关机 */
static OPENUPS_COLD bool trigger_shutdown(openups_ctx_t* restrict ctx)
{
    if (ctx == NULL || ctx->consecutive_fails < ctx->config.fail_threshold) {
        return false;
    }

    const bool use_systemctl_poweroff = ctx->config.enable_systemd && ctx->systemd_enabled;
    shutdown_trigger(&ctx->config, &ctx->logger, use_systemctl_poweroff);

    if (ctx->config.shutdown_mode == SHUTDOWN_MODE_LOG_ONLY) {
        ctx->consecutive_fails = 0; /* 重置计数器，继续监控 */
        return false;
    }

    logger_info(&ctx->logger, "Shutdown triggered, exiting monitor loop");
    return true; /* 退出监控循环 */
}

/* 执行单次监控迭代 */
static OPENUPS_HOT bool run_iteration(openups_ctx_t* restrict ctx)
{
    if (ctx == NULL) {
        return false;
    }

    /* 处理统计信息打印请求 */
    if (OPENUPS_UNLIKELY(ctx->print_stats_flag)) {
        openups_ctx_print_stats(ctx);
        ctx->print_stats_flag = 0;
    }

    /* 检查停止标志 */
    if (OPENUPS_UNLIKELY(ctx->stop_flag)) {
        return true;
    }

    /* 执行 ping */
    ping_result_t result;
    bool success = openups_ctx_ping_once(ctx, &result);

    if (OPENUPS_UNLIKELY(ctx->stop_flag)) {
        return true;
    }

    /* 处理结果 */
    if (OPENUPS_LIKELY(success)) {
        handle_ping_success(ctx, &result);
        return false;
    }

    handle_ping_failure(ctx, &result);
    return trigger_shutdown(ctx);
}

/* === 主监控循环 === */

static int openups_ctx_run(openups_ctx_t* restrict ctx)
{
    if (ctx == NULL) {
        return -1;
    }

    const config_t* config = &ctx->config;

    /* 设置信号处理 */
    setup_signal_handlers(ctx);

    /* 启动日志 */
    logger_info(&ctx->logger,
                "Starting OpenUPS monitor for target %s, checking every %d seconds, "
                "shutdown after %d consecutive failures (IPv%s)",
                config->target, config->interval_sec, config->fail_threshold,
                config->use_ipv6 ? "6" : "4");

    /* 通知 systemd ready */
    if (ctx->systemd_enabled) {
        (void)systemd_notifier_ready(&ctx->systemd);
        notify_systemd_status(ctx, "Monitoring %s, checking every %ds, threshold %d failures",
                              config->target, config->interval_sec, config->fail_threshold);
    }

    /* 主循环 */
    while (!ctx->stop_flag) {
        if (run_iteration(ctx)) {
            break;
        }

        openups_ctx_sleep_interruptible(ctx, config->interval_sec);
    }

    /* 优雅关闭 */
    if (ctx->stop_flag) {
        logger_info(&ctx->logger, "Received shutdown signal, stopping gracefully...");
        if (ctx->systemd_enabled) {
            (void)systemd_notifier_stopping(&ctx->systemd);
        }
    }

    openups_ctx_print_stats(ctx);
    logger_info(&ctx->logger, "OpenUPS monitor stopped");

    return 0;
}


/* ============================================================
 * main()
 * ============================================================ */

int main(int argc, char** argv)
{
    char error_msg[256];
    openups_ctx_t ctx;
    if (!openups_ctx_init(&ctx, argc, argv, error_msg, sizeof(error_msg))) {
        fprintf(stderr, "OpenUPS failed: %s\n", error_msg);
        return 1;
    }
    int rc = openups_ctx_run(&ctx);
    openups_ctx_destroy(&ctx);
    if (rc != 0) {
        fprintf(stderr, "OpenUPS exited with code %d\n", rc);
    }
    return rc;
}
