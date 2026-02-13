#include "base.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* ============================================================
 * common 通用工具函数
 * ============================================================ */

/* 编译时检查 */
static_assert(sizeof(uint64_t) == 8, "uint64_t must be 8 bytes");
static_assert(sizeof(time_t) >= 4, "time_t must be at least 4 bytes");

uint64_t get_timestamp_ms(void)
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

uint64_t get_monotonic_ms(void)
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

char* get_timestamp_str(char* restrict buffer, size_t size)
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

/* 从环境变量读取布尔值 (true/false, 不区分大小写) */
bool get_env_bool(const char* restrict name, bool default_value)
{
    if (name == NULL) {
        return default_value;
    }
    const char* value = getenv(name);
    if (value == NULL) {
        return default_value;
    }

    /* 仅支持 true/false */
    if (strcasecmp(value, "true") == 0) {
        return true;
    }
    if (strcasecmp(value, "false") == 0) {
        return false;
    }

    return default_value;
}

/* 从环境变量读取整数值，不正常值时返回默认值 */
int get_env_int(const char* restrict name, int default_value)
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

    /* 检查转换错误 */
    if (errno != 0 || *endptr != '\0' || result > INT_MAX || result < INT_MIN) {
        return default_value;
    }

    return (int)result;
}

/* 检验路径是否安全 (防止路径遍历和命令注入)
 * 使用 256 字节查找表实现 O(1) 危险字符检测
 */
bool is_safe_path(const char* restrict path)
{
    if (OPENUPS_UNLIKELY(path == NULL || *path == '\0')) {
        return false;
    }

    /* 静态查找表：避免每次调用 strchr 遍历危险字符串 */
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

    /* 检查路径遍历 */
    if (OPENUPS_UNLIKELY(strstr(path, "..") != NULL)) {
        return false;
    }

    return true;
}

/* ============================================================
 * logger 日志系统
 * ============================================================ */

void logger_init(logger_t* restrict logger, log_level_t level, bool enable_timestamp)
{
    if (logger == NULL) {
        return;
    }

    logger->level = level;
    logger->enable_timestamp = enable_timestamp;
}

void logger_destroy(logger_t* restrict logger)
{
    if (logger == NULL) {
        return;
    }
}

static void log_message(logger_t* restrict logger, log_level_t level, const char* restrict msg)
{
    if (OPENUPS_UNLIKELY(logger == NULL || msg == NULL)) {
        return;
    }

    /* SILENT 级别不输出任何日志；级别不足也跳过 */
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

    /* 输出到控制台（stderr 被 systemd 自动捕获） */
    if (logger->enable_timestamp) {
        fprintf(stderr, "[%s] [%s] %s\n", timestamp, level_str, msg);
    } else {
        fprintf(stderr, "[%s] %s\n", level_str, msg);
    }
}

OPENUPS_HOT void logger_debug(logger_t* restrict logger, const char* restrict fmt, ...)
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

OPENUPS_HOT void logger_info(logger_t* restrict logger, const char* restrict fmt, ...)
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

void logger_warn(logger_t* restrict logger, const char* restrict fmt, ...)
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

OPENUPS_COLD void logger_error(logger_t* restrict logger, const char* restrict fmt, ...)
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

/* 将 log_level_t 枚举转换为人类可读的字符串 */
const char* log_level_to_string(log_level_t level)
{
    switch (level) {
        case LOG_LEVEL_SILENT:
            return "SILENT";
        case LOG_LEVEL_ERROR:
            return "ERROR";
        case LOG_LEVEL_WARN:
            return "WARN";
        case LOG_LEVEL_INFO:
            return "INFO";
        case LOG_LEVEL_DEBUG:
            return "DEBUG";
        default:
            return "UNKNOWN";
    }
}

log_level_t string_to_log_level(const char* restrict str)
{
    if (str == NULL) {
        /* 默认 */
        return LOG_LEVEL_INFO;
    }

    if (strcasecmp(str, "silent") == 0 || strcasecmp(str, "none") == 0) {
        return LOG_LEVEL_SILENT;
    }
    if (strcasecmp(str, "error") == 0)
        return LOG_LEVEL_ERROR;
    if (strcasecmp(str, "warn") == 0)
        return LOG_LEVEL_WARN;
    if (strcasecmp(str, "info") == 0)
        return LOG_LEVEL_INFO;
    if (strcasecmp(str, "debug") == 0)
        return LOG_LEVEL_DEBUG;
    /* 默认 */
    return LOG_LEVEL_INFO;
}

/* ============================================================
 * metrics 指标统计
 * ============================================================ */

void metrics_init(metrics_t* metrics)
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

OPENUPS_HOT void metrics_record_success(metrics_t* metrics, double latency_ms)
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

OPENUPS_HOT void metrics_record_failure(metrics_t* metrics)
{
    if (OPENUPS_UNLIKELY(metrics == NULL)) {
        return;
    }

    metrics->total_pings++;
    metrics->failed_pings++;
}

double metrics_success_rate(const metrics_t* metrics)
{
    if (metrics == NULL || metrics->total_pings == 0) {
        return 0.0;
    }

    return (double)metrics->successful_pings / (double)metrics->total_pings * 100.0;
}

double metrics_avg_latency(const metrics_t* metrics)
{
    if (metrics == NULL || metrics->successful_pings == 0) {
        return 0.0;
    }

    return metrics->total_latency / (double)metrics->successful_pings;
}

uint64_t metrics_uptime_seconds(const metrics_t* metrics)
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
