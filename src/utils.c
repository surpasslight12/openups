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
