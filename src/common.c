#include "common.h"
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
#include <stdckdint.h>
#else
/* Fallback for GCC < 14: basic overflow detection for unsigned types */
static inline bool ckd_add_impl(uint64_t* result, uint64_t a, uint64_t b) {
    if (b > UINT64_MAX - a) {
        return true;  /* Overflow detected */
    }
    *result = a + b;
    return false;
}

static inline bool ckd_mul_impl(uint64_t* result, uint64_t a, uint64_t b) {
    if (a != 0 && b > UINT64_MAX / a) {
        return true;  /* Overflow detected */
    }
    *result = a * b;
    return false;
}

#define ckd_add(result, a, b) ckd_add_impl(result, a, b)
#define ckd_mul(result, a, b) ckd_mul_impl(result, a, b)
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>

/* 编译时检查 */
static_assert(sizeof(uint64_t) == 8, "uint64_t must be 8 bytes");
static_assert(sizeof(time_t) >= 4, "time_t must be at least 4 bytes");

uint64_t get_timestamp_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);

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

char* get_timestamp_str(char* restrict buffer, size_t size) {
    if (buffer == nullptr || size == 0) {
        return nullptr;
    }
    
    time_t now = time(nullptr);
    struct tm* tm_info = localtime(&now);
    struct timeval tv;
    gettimeofday(&tv, nullptr);

    if (tm_info == nullptr) {
        buffer[0] = '\0';
        return buffer;
    }
    
    snprintf(buffer, size, "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
             tm_info->tm_year + 1900,
             tm_info->tm_mon + 1,
             tm_info->tm_mday,
             tm_info->tm_hour,
             tm_info->tm_min,
             tm_info->tm_sec,
             tv.tv_usec / 1000);
    
    return buffer;
}

const char* get_env_or_default(const char* restrict name, const char* restrict default_value) {
    if (name == nullptr) {
        return default_value;
    }
    const char* value = getenv(name);
    return value != nullptr ? value : default_value;
}

bool get_env_bool(const char* restrict name, bool default_value) {
    if (name == nullptr) {
        return default_value;
    }
    const char* value = getenv(name);
    if (value == nullptr) {
        return default_value;
    }
    
    /* 支持多种格式：true/false, yes/no, 1/0, on/off */
    if (strcasecmp(value, "true") == 0 || strcasecmp(value, "yes") == 0 || 
        strcmp(value, "1") == 0 || strcasecmp(value, "on") == 0) {
        return true;
    }
    if (strcasecmp(value, "false") == 0 || strcasecmp(value, "no") == 0 || 
        strcmp(value, "0") == 0 || strcasecmp(value, "off") == 0) {
        return false;
    }
    
    return default_value;
}

int get_env_int(const char* restrict name, int default_value) {
    if (name == nullptr) {
        return default_value;
    }
    const char* value = getenv(name);
    if (value == nullptr) {
        return default_value;
    }
    
    char* endptr = nullptr;
    errno = 0;
    long result = strtol(value, &endptr, 10);
    
    /* 检查转换错误 */
    if (errno != 0 || *endptr != '\0' || result > INT_MAX || result < INT_MIN) {
        return default_value;
    }
    
    return (int)result;
}

char* trim_whitespace(char* restrict str) {
    if (str == nullptr) {
        return nullptr;
    }
    
    /* 跳过前导空白 */
    while (isspace((unsigned char)*str)) str++;
    
    if (*str == '\0') {
        return str;
    }
    
    /* 移除尾部空白 */
    char* end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    
    *(end + 1) = '\0';
    
    return str;
}

bool str_equals(const char* restrict a, const char* restrict b) {
    if (a == b) {
        return true;
    }
    if (a == nullptr || b == nullptr) {
        return false;
    }
    return strcmp(a, b) == 0;
}

bool is_safe_path(const char* restrict path) {
    if (path == nullptr || *path == '\0') {
        return false;
    }
    
    /* 检查危险字符 */
    static const char dangerous[] = ";|&$`<>\"'(){}[]!\\*?";
    for (const char* p = path; *p != '\0'; ++p) {
        if (strchr(dangerous, *p) != nullptr) {
            return false;
        }
    }
    
    /* 检查路径遍历 */
    if (strstr(path, "..") != nullptr) {
        return false;
    }
    
    return true;
}
