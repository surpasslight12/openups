#include "common.h"
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/time.h>

/* 编译时检查 */
static_assert(sizeof(uint64_t) == 8, "uint64_t must be 8 bytes");
static_assert(sizeof(time_t) >= 4, "time_t must be at least 4 bytes");

uint64_t get_timestamp_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    uint64_t seconds_ms = 0;
    if (ckd_mul(&seconds_ms, (uint64_t)tv.tv_sec, UINT64_C(1000))) {
        return UINT64_MAX;
    }

    uint64_t usec_ms = (uint64_t)tv.tv_usec / 1000;
    uint64_t timestamp = 0;
    if (ckd_add(&timestamp, seconds_ms, usec_ms)) {
        return UINT64_MAX;
    }

    return timestamp;
}

uint64_t get_monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return UINT64_MAX;
    }

    uint64_t seconds_ms = 0;
    if (ckd_mul(&seconds_ms, (uint64_t)ts.tv_sec, UINT64_C(1000))) {
        return UINT64_MAX;
    }

    uint64_t nsec_ms = (uint64_t)ts.tv_nsec / UINT64_C(1000000);
    uint64_t timestamp = 0;
    if (ckd_add(&timestamp, seconds_ms, nsec_ms)) {
        return UINT64_MAX;
    }

    return timestamp;
}

char* get_timestamp_str(char* restrict buffer, size_t size)
{
    if (buffer == NULL || size == 0) {
        return NULL;
    }

    time_t now = time(NULL);
    struct tm tm_info;
    if (localtime_r(&now, &tm_info) == NULL) {
        buffer[0] = '\0';
        return buffer;
    }

    struct timeval tv;
    gettimeofday(&tv, NULL);

    snprintf(buffer, size, "%04d-%02d-%02d %02d:%02d:%02d.%03ld", tm_info.tm_year + 1900,
             tm_info.tm_mon + 1, tm_info.tm_mday, tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec,
             tv.tv_usec / 1000);

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

/* 检验路径是否安全 (防止路径遍历和命令注入) */
bool is_safe_path(const char* restrict path)
{
    if (path == NULL || *path == '\0') {
        return false;
    }

    /* 检查危险字符 */
    static const char dangerous[] = ";|&$`<>\"'(){}[]!\\*?";
    for (const char* p = path; *p != '\0'; ++p) {
        if (strchr(dangerous, *p) != NULL) {
            return false;
        }
    }

    /* 检查路径遍历 */
    if (strstr(path, "..") != NULL) {
        return false;
    }

    return true;
}
