#include "openups.h"

/* Monotonic clock in milliseconds; returns UINT64_MAX on overflow or error. */
uint64_t get_monotonic_ms(void) {
  struct timespec ts;
#ifdef CLOCK_MONOTONIC_COARSE
  if (OPENUPS_UNLIKELY(clock_gettime(CLOCK_MONOTONIC_COARSE, &ts) != 0)) {
    if (OPENUPS_UNLIKELY(clock_gettime(CLOCK_MONOTONIC, &ts) != 0)) {
      return UINT64_MAX;
    }
  }
#else
  if (OPENUPS_UNLIKELY(clock_gettime(CLOCK_MONOTONIC, &ts) != 0)) {
    return UINT64_MAX;
  }
#endif
  uint64_t seconds_ms = 0;
  if (OPENUPS_UNLIKELY(
          ckd_mul(&seconds_ms, (uint64_t)ts.tv_sec, UINT64_C(1000)))) {
    return UINT64_MAX;
  }
  uint64_t nsec_ms = (uint64_t)ts.tv_nsec / UINT64_C(1000000);
  uint64_t timestamp = 0;
  if (OPENUPS_UNLIKELY(ckd_add(&timestamp, seconds_ms, nsec_ms))) {
    return UINT64_MAX;
  }
  return timestamp;
}

char *get_timestamp_str(char *restrict buffer, size_t size) {
  if (OPENUPS_UNLIKELY(buffer == NULL || size == 0)) {
    return NULL;
  }
  struct timespec ts;
  if (OPENUPS_UNLIKELY(clock_gettime(CLOCK_REALTIME, &ts) != 0)) {
    buffer[0] = '\0';
    return buffer;
  }
  struct tm tm_info;
  if (OPENUPS_UNLIKELY(localtime_r(&ts.tv_sec, &tm_info) == NULL)) {
    buffer[0] = '\0';
    return buffer;
  }
  snprintf(buffer, size, "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
           tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
           tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec,
           ts.tv_nsec / 1000000);
  return buffer;
}

bool get_env_bool(const char *restrict name, bool default_value) {
  if (name == NULL) {
    return default_value;
  }
  const char *value = getenv(name);
  if (value == NULL) {
    return default_value;
  }
  if (strcasecmp(value, "true") == 0) {
    return true;
  }
  if (strcasecmp(value, "false") == 0) {
    return false;
  }
  return default_value;
}

int get_env_int(const char *restrict name, int default_value) {
  if (name == NULL) {
    return default_value;
  }
  const char *value = getenv(name);
  if (value == NULL) {
    return default_value;
  }
  char *endptr = NULL;
  errno = 0;
  long result = strtol(value, &endptr, 10);
  if (errno != 0 || *endptr != '\0' || result > INT_MAX || result < INT_MIN) {
    return default_value;
  }
  return (int)result;
}

bool is_safe_path(const char *restrict path) {
  if (OPENUPS_UNLIKELY(path == NULL || *path == '\0')) {
    return false;
  }
  static const bool dangerous[256] = {
      [';'] = true, ['|'] = true,  ['&'] = true, ['$'] = true,  ['`'] = true,
      ['<'] = true, ['>'] = true,  ['"'] = true, ['\''] = true, ['('] = true,
      [')'] = true, ['{'] = true,  ['}'] = true, ['['] = true,  [']'] = true,
      ['!'] = true, ['\\'] = true, ['*'] = true, ['?'] = true,
  };
  for (const char *p = path; *p != '\0'; ++p) {
    if (OPENUPS_UNLIKELY(dangerous[(unsigned char)*p])) {
      return false;
    }
  }
  if (OPENUPS_UNLIKELY(strstr(path, "..") != NULL)) {
    return false;
  }
  return true;
}

log_level_t string_to_log_level(const char *restrict str) {
  if (str == NULL) {
    return LOG_LEVEL_INFO;
  }
  if (strcasecmp(str, "silent") == 0 || strcasecmp(str, "none") == 0) {
    return LOG_LEVEL_SILENT;
  }
  if (strcasecmp(str, "error") == 0) {
    return LOG_LEVEL_ERROR;
  }
  if (strcasecmp(str, "warn") == 0) {
    return LOG_LEVEL_WARN;
  }
  if (strcasecmp(str, "info") == 0) {
    return LOG_LEVEL_INFO;
  }
  if (strcasecmp(str, "debug") == 0) {
    return LOG_LEVEL_DEBUG;
  }
  return LOG_LEVEL_INFO;
}
