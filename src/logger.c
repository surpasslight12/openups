#include "openups.h"

void logger_init(logger_t *restrict logger, log_level_t level,
                 bool enable_timestamp) {
  if (logger == NULL) {
    return;
  }
  logger->level = level;
  logger->enable_timestamp = enable_timestamp;
}

const char *log_level_to_string(log_level_t level) {
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

static void log_message(const logger_t *restrict logger, log_level_t level,
                        const char *restrict msg) {
  if (OPENUPS_UNLIKELY(logger == NULL || msg == NULL)) {
    return;
  }
  if (OPENUPS_UNLIKELY(logger->level == LOG_LEVEL_SILENT)) {
    return;
  }
  char timestamp[64];
  if (logger->enable_timestamp) {
    if (get_timestamp_str(timestamp, sizeof(timestamp)) == NULL) {
      timestamp[0] = '\0';
    }
  }
  const char *level_str = log_level_to_string(level);
  if (logger->enable_timestamp) {
    fprintf(stderr, "[%s] [%s] %s\n", timestamp, level_str, msg);
  } else {
    fprintf(stderr, "[%s] %s\n", level_str, msg);
  }
}

void logger_log_va(const logger_t *restrict logger, log_level_t level,
                   const char *restrict fmt, va_list ap) {
  char buffer[OPENUPS_LOG_BUFFER_SIZE];
  vsnprintf(buffer, sizeof(buffer), fmt, ap);
  log_message(logger, level, buffer);
}

void log_shutdown_countdown(const logger_t *restrict logger,
                            shutdown_mode_t mode, int delay_minutes) {
  if (logger == NULL) {
    return;
  }

  if (mode == SHUTDOWN_MODE_DRY_RUN) {
    logger_warn(logger, "Starting dry-run countdown for %d minutes",
                delay_minutes);
  } else if (mode == SHUTDOWN_MODE_TRUE_OFF) {
    logger_warn(logger, "Starting true-off countdown for %d minutes",
                delay_minutes);
  }
}
