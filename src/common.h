#ifndef OPENUPS_COMMON_H
#define OPENUPS_COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

/* 版本信息 */
#define VERSION "1.1.0"
#define PROGRAM_NAME "openups"

/* 获取当前时间戳（毫秒级） */
uint64_t get_timestamp_ms(void);

/* 获取格式化时间字符串 */
char* get_timestamp_str(char* buffer, size_t size);

/* 环境变量读取 */
const char* get_env_or_default(const char* name, const char* default_value);
bool get_env_bool(const char* name, bool default_value);
int get_env_int(const char* name, int default_value);

/* 字符串工具 */
char* trim_whitespace(char* str);
bool str_equals(const char* a, const char* b);
bool is_safe_path(const char* path);

#endif /* OPENUPS_COMMON_H */
