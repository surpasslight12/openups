#ifndef OPENUPS_COMMON_H
#define OPENUPS_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* 版本信息 */
#define VERSION "1.2.0"
#define PROGRAM_NAME "openups"

/* 获取当前时间戳（毫秒级） */
[[nodiscard]] uint64_t get_timestamp_ms(void);

/* 获取格式化时间字符串 */
[[nodiscard]] char* get_timestamp_str(char* restrict buffer, size_t size);

/* 环境变量读取 */
[[nodiscard]] const char* get_env_or_default(const char* restrict name, const char* restrict default_value);
[[nodiscard]] bool get_env_bool(const char* restrict name, bool default_value);
[[nodiscard]] int get_env_int(const char* restrict name, int default_value);

/* 字符串工具 */
[[nodiscard]] char* trim_whitespace(char* restrict str);
[[nodiscard]] bool str_equals(const char* restrict a, const char* restrict b);
[[nodiscard]] bool is_safe_path(const char* restrict path);

#endif /* OPENUPS_COMMON_H */
