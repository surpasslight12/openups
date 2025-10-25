#ifndef OPENUPS_LOGGER_H
#define OPENUPS_LOGGER_H

#include <stdbool.h>

/* 日志级别 */
typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR
} log_level_t;

/* 日志器结构 */
typedef struct {
    log_level_t level;
    bool verbose;
    bool enable_timestamp;
    bool use_syslog;
} logger_t;

/* 初始化日志器 */
void logger_init(logger_t* logger, log_level_t level, bool verbose, 
                 bool enable_timestamp, bool use_syslog);

/* 销毁日志器 */
void logger_destroy(logger_t* logger);

/* 基本日志 */
void logger_debug(logger_t* logger, const char* msg);
void logger_info(logger_t* logger, const char* msg);
void logger_warn(logger_t* logger, const char* msg);
void logger_error(logger_t* logger, const char* msg);

/* 带键值对的结构化日志 */
void logger_debug_kv(logger_t* logger, const char* msg, const char** keys, const char** values, int count);
void logger_info_kv(logger_t* logger, const char* msg, const char** keys, const char** values, int count);
void logger_warn_kv(logger_t* logger, const char* msg, const char** keys, const char** values, int count);
void logger_error_kv(logger_t* logger, const char* msg, const char** keys, const char** values, int count);

/* 日志级别转换 */
const char* log_level_to_string(log_level_t level);
log_level_t string_to_log_level(const char* str);

#endif /* OPENUPS_LOGGER_H */
