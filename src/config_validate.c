#include "config_internal.h"

#include <arpa/inet.h>
#include <stdio.h>

static bool set_error(char *restrict error_msg, size_t error_size,
                      const char *restrict fmt, ...)
    __attribute__((format(printf, 3, 4)));

static bool set_error(char *restrict error_msg, size_t error_size,
                      const char *restrict fmt, ...) {
  if (error_msg == NULL || error_size == 0 || fmt == NULL) {
    return false;
  }

  va_list args;
  va_start(args, fmt);
  vsnprintf(error_msg, error_size, fmt, args);
  va_end(args);
  return false;
}

static bool is_valid_ip_literal(const char *restrict target) {
  if (target == NULL || target[0] == '\0') {
    return false;
  }

  unsigned char addr_buffer[sizeof(struct in6_addr)];
  if (inet_pton(AF_INET, target, addr_buffer) == 1) {
    return true;
  }
  if (inet_pton(AF_INET6, target, addr_buffer) == 1) {
    return true;
  }
  return false;
}

static bool timeout_fits_interval(const config_t *restrict config) {
  if (config == NULL || config->interval_sec <= 0 || config->timeout_ms <= 0) {
    return false;
  }

  uint64_t interval_ms = 0;
  if (ckd_mul(&interval_ms, (uint64_t)config->interval_sec,
              OPENUPS_MS_PER_SEC)) {
    return false;
  }

  return (uint64_t)config->timeout_ms < interval_ms;
}

bool config_validate(const config_t *restrict config, char *restrict error_msg,
                     size_t error_size) {
  if (config == NULL || error_msg == NULL || error_size == 0) {
    return false;
  }

  if (config->target[0] == '\0') {
    return set_error(error_msg, error_size, "Target host cannot be empty");
  }
  if (!is_valid_ip_literal(config->target)) {
    return set_error(
        error_msg, error_size,
        "Target must be a valid IPv4 or IPv6 address (DNS is disabled)");
  }
  if (config->interval_sec <= 0) {
    return set_error(error_msg, error_size, "Interval must be positive");
  }
  if (config->fail_threshold <= 0) {
    return set_error(error_msg, error_size,
                     "Failure threshold must be positive");
  }
  if (config->timeout_ms <= 0) {
    return set_error(error_msg, error_size, "Timeout must be positive");
  }
  if (config->delay_minutes < 0) {
    return set_error(error_msg, error_size,
                     "Delay minutes cannot be negative");
  }
  if (config->delay_minutes > OPENUPS_MAX_DELAY_MINUTES) {
    return set_error(error_msg, error_size,
                     "Delay minutes too large (max 525600)");
  }
  if (!timeout_fits_interval(config)) {
    return set_error(error_msg, error_size,
                     "Timeout must be smaller than interval to avoid overlapping probes");
  }
  if (config->shutdown_mode == SHUTDOWN_MODE_LOG_ONLY &&
      config->delay_minutes != 0) {
    return set_error(error_msg, error_size,
                     "Delay is only valid with dry-run or true-off shutdown modes");
  }

  return true;
}