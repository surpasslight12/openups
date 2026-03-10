#ifndef OPENUPS_RUNTIME_SERVICES_H
#define OPENUPS_RUNTIME_SERVICES_H

#include "openups.h"

bool runtime_services_systemd_enabled(const openups_ctx_t *restrict ctx);
uint64_t runtime_services_watchdog_interval_ms(
    const openups_ctx_t *restrict ctx);
void runtime_services_notify_statusf(openups_ctx_t *restrict ctx,
                                     const char *restrict fmt, ...);
bool runtime_services_notify_ready(openups_ctx_t *restrict ctx);
bool runtime_services_notify_watchdog(openups_ctx_t *restrict ctx);
bool runtime_services_notify_stopping(openups_ctx_t *restrict ctx);

#endif