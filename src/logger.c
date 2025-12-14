#include "logger.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <stdarg.h>

void logger_init(logger_t* restrict logger, log_level_t level,
                 bool enable_timestamp) {
    if (logger == nullptr) {
        return;
    }
    
    logger->level = level;
    logger->enable_timestamp = enable_timestamp;
}

void logger_destroy(logger_t* restrict logger) {
    if (logger == nullptr) {
        return;
    }
}

static void log_message(logger_t* restrict logger, log_level_t level, const char* restrict msg) {
    if (logger == nullptr || msg == nullptr) {
        return;
    }
    
    /* SILENT 级别不输出任何日志 */
    if (logger->level == LOG_LEVEL_SILENT) {
        return;
    }
    
    /* 检查日志级别 */
    if (level > logger->level) {
        return;
    }
    
    char timestamp[64];
    if (logger->enable_timestamp) {
        if (get_timestamp_str(timestamp, sizeof(timestamp)) == nullptr) {
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

void logger_debug(logger_t* restrict logger, const char* restrict fmt, ...) {
    if (logger == nullptr || fmt == nullptr || logger->level < LOG_LEVEL_DEBUG) {
        return;
    }
    
    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    
    log_message(logger, LOG_LEVEL_DEBUG, buffer);
}

void logger_info(logger_t* restrict logger, const char* restrict fmt, ...) {
    if (logger == nullptr || fmt == nullptr || logger->level < LOG_LEVEL_INFO) {
        return;
    }
    
    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    
    log_message(logger, LOG_LEVEL_INFO, buffer);
}

void logger_warn(logger_t* restrict logger, const char* restrict fmt, ...) {
    if (logger == nullptr || fmt == nullptr || logger->level < LOG_LEVEL_WARN) {
        return;
    }
    
    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    
    log_message(logger, LOG_LEVEL_WARN, buffer);
}

void logger_error(logger_t* restrict logger, const char* restrict fmt, ...) {
    if (logger == nullptr || fmt == nullptr || logger->level < LOG_LEVEL_ERROR) {
        return;
    }
    
    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    
    log_message(logger, LOG_LEVEL_ERROR, buffer);
}

/* 将 log_level_t 枚举转换为人类可读的字符串 */
const char* log_level_to_string(log_level_t level) {
    switch (level) {
        case LOG_LEVEL_SILENT: return "SILENT";
        case LOG_LEVEL_ERROR:  return "ERROR";
        case LOG_LEVEL_WARN:   return "WARN";
        case LOG_LEVEL_INFO:   return "INFO";
        case LOG_LEVEL_DEBUG:  return "DEBUG";
        default:               return "UNKNOWN";
    }
}

log_level_t string_to_log_level(const char* restrict str) {
    if (str == nullptr) {
        /* 默认 */
        return LOG_LEVEL_INFO;
    }
    
    if (strcasecmp(str, "silent") == 0 || strcasecmp(str, "none") == 0) 
        return LOG_LEVEL_SILENT;
    if (strcasecmp(str, "error") == 0) return LOG_LEVEL_ERROR;
    if (strcasecmp(str, "warn") == 0)  return LOG_LEVEL_WARN;
    if (strcasecmp(str, "info") == 0)  return LOG_LEVEL_INFO;
    if (strcasecmp(str, "debug") == 0) return LOG_LEVEL_DEBUG;
    /* 默认 */
    return LOG_LEVEL_INFO;
}
