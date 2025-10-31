#ifndef OPENUPS_LOGGER_H
#define OPENUPS_LOGGER_H

#include <stdbool.h>

/* 日志级别 */
typedef enum {
    LOG_LEVEL_SILENT = -1,  /* 完全静默，不输出任何日志 */
    LOG_LEVEL_ERROR = 0,    /* 仅输出错误 */
    LOG_LEVEL_WARN = 1,     /* 输出警告和错误 */
    LOG_LEVEL_INFO = 2,     /* 输出信息、警告和错误（默认） */
    LOG_LEVEL_DEBUG = 3     /* 输出所有日志，包括调试信息 */
} log_level_t;

/* 日志器结构 */
typedef struct {
    log_level_t level;
    bool enable_timestamp;
    bool use_syslog;
} logger_t;

/* 初始化日志器 */
void logger_init(logger_t* restrict logger, log_level_t level,
                 bool enable_timestamp, bool use_syslog);

/* 销毁日志器 */
void logger_destroy(logger_t* restrict logger);

/* 基本日志函数（使用 printf 风格格式化）*/
void logger_debug(logger_t* restrict logger, const char* restrict fmt, ...) __attribute__((format(printf, 2, 3)));
void logger_info(logger_t* restrict logger, const char* restrict fmt, ...) __attribute__((format(printf, 2, 3)));
void logger_warn(logger_t* restrict logger, const char* restrict fmt, ...) __attribute__((format(printf, 2, 3)));
void logger_error(logger_t* restrict logger, const char* restrict fmt, ...) __attribute__((format(printf, 2, 3)));

/* 日志级别转换 */
[[nodiscard]] const char* log_level_to_string(log_level_t level);
[[nodiscard]] log_level_t string_to_log_level(const char* restrict str);

#endif /* OPENUPS_LOGGER_H */
