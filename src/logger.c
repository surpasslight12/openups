#include "logger.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>
#include <time.h>

void logger_init(logger_t* logger, log_level_t level, bool verbose,
                 bool enable_timestamp, bool use_syslog) {
    logger->level = level;
    logger->verbose = verbose;
    logger->enable_timestamp = enable_timestamp;
    logger->use_syslog = use_syslog;
    
    if (use_syslog) {
        openlog(PROGRAM_NAME, LOG_PID | LOG_CONS, LOG_USER);
    }
}

void logger_destroy(logger_t* logger) {
    if (logger->use_syslog) {
        closelog();
    }
}

static int level_to_syslog_priority(log_level_t level) {
    switch (level) {
        case LOG_LEVEL_DEBUG: return LOG_DEBUG;
        case LOG_LEVEL_INFO:  return LOG_INFO;
        case LOG_LEVEL_WARN:  return LOG_WARNING;
        case LOG_LEVEL_ERROR: return LOG_ERR;
        default:              return LOG_INFO;
    }
}

static void log_message(logger_t* logger, log_level_t level, const char* msg) {
    if (level < logger->level) {
        return;
    }
    
    char timestamp[64];
    if (logger->enable_timestamp) {
        get_timestamp_str(timestamp, sizeof(timestamp));
    }
    
    const char* level_str = log_level_to_string(level);
    
    /* 输出到控制台 */
    if (logger->enable_timestamp) {
        fprintf(stderr, "[%s] [%s] %s\n", timestamp, level_str, msg);
    } else {
        fprintf(stderr, "[%s] %s\n", level_str, msg);
    }
    
    /* 输出到 syslog */
    if (logger->use_syslog) {
        syslog(level_to_syslog_priority(level), "%s", msg);
    }
}

static void log_message_kv(logger_t* logger, log_level_t level, const char* msg,
                           const char** keys, const char** values, int count) {
    if (level < logger->level) {
        return;
    }
    
    /* 构建带键值对的消息 */
    char buffer[2048];
    int offset = snprintf(buffer, sizeof(buffer), "%s", msg);
    
    for (int i = 0; i < count && offset < (int)sizeof(buffer) - 1; i++) {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                          " %s=%s", keys[i], values[i]);
    }
    
    log_message(logger, level, buffer);
}

void logger_debug(logger_t* logger, const char* msg) {
    log_message(logger, LOG_LEVEL_DEBUG, msg);
}

void logger_info(logger_t* logger, const char* msg) {
    log_message(logger, LOG_LEVEL_INFO, msg);
}

void logger_warn(logger_t* logger, const char* msg) {
    log_message(logger, LOG_LEVEL_WARN, msg);
}

void logger_error(logger_t* logger, const char* msg) {
    log_message(logger, LOG_LEVEL_ERROR, msg);
}

void logger_debug_kv(logger_t* logger, const char* msg, const char** keys,
                     const char** values, int count) {
    log_message_kv(logger, LOG_LEVEL_DEBUG, msg, keys, values, count);
}

void logger_info_kv(logger_t* logger, const char* msg, const char** keys,
                    const char** values, int count) {
    log_message_kv(logger, LOG_LEVEL_INFO, msg, keys, values, count);
}

void logger_warn_kv(logger_t* logger, const char* msg, const char** keys,
                    const char** values, int count) {
    log_message_kv(logger, LOG_LEVEL_WARN, msg, keys, values, count);
}

void logger_error_kv(logger_t* logger, const char* msg, const char** keys,
                     const char** values, int count) {
    log_message_kv(logger, LOG_LEVEL_ERROR, msg, keys, values, count);
}

const char* log_level_to_string(log_level_t level) {
    switch (level) {
        case LOG_LEVEL_DEBUG: return "DEBUG";
        case LOG_LEVEL_INFO:  return "INFO";
        case LOG_LEVEL_WARN:  return "WARN";
        case LOG_LEVEL_ERROR: return "ERROR";
        default:              return "UNKNOWN";
    }
}

log_level_t string_to_log_level(const char* str) {
    if (strcasecmp(str, "debug") == 0) return LOG_LEVEL_DEBUG;
    if (strcasecmp(str, "info") == 0)  return LOG_LEVEL_INFO;
    if (strcasecmp(str, "warn") == 0)  return LOG_LEVEL_WARN;
    if (strcasecmp(str, "error") == 0) return LOG_LEVEL_ERROR;
    return LOG_LEVEL_INFO;
}
