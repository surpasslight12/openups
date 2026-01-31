#ifndef INTEGRATIONS_H
#define INTEGRATIONS_H

/**
 * @file integrations.h
 * @brief 系统集成：systemd 通知、关机触发
 */

#include "config.h"

#include <stdbool.h>
#include <stdint.h>

#include <sys/socket.h>
#include <sys/un.h>

/* === systemd 通知器 === */

typedef struct {
    bool enabled;
    int sockfd;
    uint64_t watchdog_usec;
    struct sockaddr_un addr;
    socklen_t addr_len;

    uint64_t last_watchdog_ms;
    uint64_t last_status_ms;
    char last_status[256];
} systemd_notifier_t;

void systemd_notifier_init(systemd_notifier_t* restrict notifier);
void systemd_notifier_destroy(systemd_notifier_t* restrict notifier);
[[nodiscard]] bool systemd_notifier_is_enabled(const systemd_notifier_t* restrict notifier);
[[nodiscard]] bool systemd_notifier_ready(systemd_notifier_t* restrict notifier);
[[nodiscard]] bool systemd_notifier_status(systemd_notifier_t* restrict notifier,
                                           const char* restrict status);
[[nodiscard]] bool systemd_notifier_stopping(systemd_notifier_t* restrict notifier);
[[nodiscard]] bool systemd_notifier_watchdog(systemd_notifier_t* restrict notifier);
[[nodiscard]] uint64_t systemd_notifier_watchdog_interval_ms(
    const systemd_notifier_t* restrict notifier);

void shutdown_trigger(const config_t* config, logger_t* logger, bool use_systemctl_poweroff);

#endif /* INTEGRATIONS_H */
