#ifndef OPENUPS_COMMON_H
#define OPENUPS_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

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

/* 版本信息 */
#define VERSION "1.2.0"
#define PROGRAM_NAME "openups"

/* 获取当前时间戳（毫秒级） */
[[nodiscard]] uint64_t get_timestamp_ms(void);

/* 获取单调时钟（毫秒级，用于延迟/运行时长统计，避免系统时间跳变） */
[[nodiscard]] uint64_t get_monotonic_ms(void);

/* 获取格式化时间字符串 */
[[nodiscard]] char* get_timestamp_str(char* restrict buffer, size_t size);

/* 环境变量读取 */
[[nodiscard]] bool get_env_bool(const char* restrict name, bool default_value);
[[nodiscard]] int get_env_int(const char* restrict name, int default_value);

/* 字符串工具 */
[[nodiscard]] bool is_safe_path(const char* restrict path);

#endif /* OPENUPS_COMMON_H */
