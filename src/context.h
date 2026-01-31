#ifndef CONTEXT_H
#define CONTEXT_H

/**
 * @file context.h
 * @brief 统一上下文管理：配置、组件、监控循环、信号处理
 */

#include "base.h"
#include "config.h"
#include "icmp.h"
#include "integrations.h"

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>

/* === 统一上下文结构 === */

typedef struct openups_context {
    volatile sig_atomic_t stop_flag;
    volatile sig_atomic_t print_stats_flag;
    int consecutive_fails;

    config_t config;
    logger_t logger;
    icmp_pinger_t pinger;
    systemd_notifier_t systemd;
    metrics_t metrics;

    bool systemd_enabled;
    uint64_t watchdog_interval_ms;

    uint64_t last_ping_time_ms;
    uint64_t start_time_ms;
} openups_ctx_t;

[[nodiscard]] bool openups_ctx_init(openups_ctx_t* restrict ctx, int argc,
                                    char** restrict argv, char* restrict error_msg,
                                    size_t error_size);
void openups_ctx_destroy(openups_ctx_t* restrict ctx);
[[nodiscard]] int openups_ctx_run(openups_ctx_t* restrict ctx);
void openups_ctx_print_stats(openups_ctx_t* restrict ctx);
[[nodiscard]] bool openups_ctx_ping_once(openups_ctx_t* restrict ctx,
                                         ping_result_t* restrict result);
void openups_ctx_sleep_interruptible(openups_ctx_t* restrict ctx, int seconds);

#endif /* CONTEXT_H */
