#ifndef BASE_H
#define BASE_H

/**
 * @file base.h
 * @brief 基础设施：通用工具、日志系统、指标统计
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "openups.h"

/* === C23 checked arithmetic（stdckdint.h）=== */
/* 不支持时提供等价回退实现 */
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

/* ===== common ===== */

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

#endif /* BASE_H */
