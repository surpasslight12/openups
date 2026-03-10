#include "runtime_services.h"

#include <stdarg.h>
#include <stdio.h>

bool runtime_services_systemd_enabled(const openups_ctx_t *restrict ctx) {
  return ctx != NULL && systemd_notifier_is_enabled(&ctx->systemd);
}

uint64_t runtime_services_watchdog_interval_ms(
    const openups_ctx_t *restrict ctx) {
  if (ctx == NULL) {
    return 0;
  }

  return systemd_notifier_watchdog_interval_ms(&ctx->systemd);
}

void runtime_services_notify_statusf(openups_ctx_t *restrict ctx,
                                     const char *restrict fmt, ...) {
  if (ctx == NULL || !runtime_services_systemd_enabled(ctx) || fmt == NULL) {
    return;
  }

  char status_msg[OPENUPS_SYSTEMD_STATUS_SIZE];
  va_list args;
  va_start(args, fmt);
  vsnprintf(status_msg, sizeof(status_msg), fmt, args);
  va_end(args);

  (void)systemd_notifier_status(&ctx->systemd, status_msg);
}

bool runtime_services_notify_ready(openups_ctx_t *restrict ctx) {
  return ctx != NULL && systemd_notifier_ready(&ctx->systemd);
}

bool runtime_services_notify_watchdog(openups_ctx_t *restrict ctx) {
  return ctx != NULL && systemd_notifier_watchdog(&ctx->systemd);
}

bool runtime_services_notify_stopping(openups_ctx_t *restrict ctx) {
  return ctx != NULL && systemd_notifier_stopping(&ctx->systemd);
}