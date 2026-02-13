#include "context.h"
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* 编译时检查 */
static_assert(sizeof(sig_atomic_t) >= sizeof(int), "sig_atomic_t must be at least int size");

/* === 全局上下文指针（仅用于信号处理） === */
static openups_ctx_t* g_ctx = NULL;

/* === 信号处理 === */

static void signal_handler(int signum)
{
    if (g_ctx == NULL) {
        return;
    }

    switch (signum) {
    case SIGINT:
    case SIGTERM:
        g_ctx->stop_flag = 1;
        break;
    case SIGUSR1:
        g_ctx->print_stats_flag = 1;
        break;
    default:
        break;
    }
}

static void setup_signal_handlers(openups_ctx_t* restrict ctx)
{
    if (ctx == NULL) {
        return;
    }

    g_ctx = ctx;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);

    (void)sigaction(SIGINT, &sa, NULL);
    (void)sigaction(SIGTERM, &sa, NULL);
    (void)sigaction(SIGUSR1, &sa, NULL);
}

/* === 内部辅助函数 === */

/* watchdog 心跳回调（在 ICMP 等待期间定期发送） */
static void watchdog_tick_callback(void* user_data)
{
#ifdef OPENUPS_HAS_SYSTEMD
    openups_ctx_t* ctx = (openups_ctx_t*)user_data;
    if (ctx == NULL || !ctx->config.enable_watchdog || !ctx->systemd_enabled) {
        return;
    }

    (void)systemd_notifier_watchdog(&ctx->systemd);
#else
    (void)user_data;
#endif
}

/* 监控停止条件回调（ICMP 等待期间检查是否应中断） */
static bool monitor_should_stop(void* user_data)
{
    openups_ctx_t* ctx = (openups_ctx_t*)user_data;
    return ctx != NULL && ctx->stop_flag != 0;
}

#ifdef OPENUPS_HAS_SYSTEMD
/* systemd 状态通知辅助函数 */
static void notify_systemd_status(openups_ctx_t* restrict ctx, const char* restrict fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void notify_systemd_status(openups_ctx_t* restrict ctx, const char* restrict fmt, ...)
{
    if (ctx == NULL || !ctx->systemd_enabled || fmt == NULL) {
        return;
    }

    char status_msg[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(status_msg, sizeof(status_msg), fmt, args);
    va_end(args);

    (void)systemd_notifier_status(&ctx->systemd, status_msg);
}
#endif /* OPENUPS_HAS_SYSTEMD */

/* === 上下文初始化 === */

bool openups_ctx_init(openups_ctx_t* restrict ctx, int argc, char** restrict argv,
                      char* restrict error_msg, size_t error_size)
{
    if (ctx == NULL || error_msg == NULL || error_size == 0) {
        return false;
    }

    /* 零初始化 */
    memset(ctx, 0, sizeof(*ctx));
    ctx->start_time_ms = get_monotonic_ms();

    /* === 配置加载（3 层优先级） === */
    config_init_default(&ctx->config);
    config_load_from_env(&ctx->config);

    if (!config_load_from_cmdline(&ctx->config, argc, argv)) {
        snprintf(error_msg, error_size, "Failed to parse command line arguments");
        return false;
    }

    if (!config_validate(&ctx->config, error_msg, error_size)) {
        return false;
    }

    /* === 日志器初始化 === */
    logger_init(&ctx->logger, ctx->config.log_level, ctx->config.enable_timestamp);

    /* 调试模式下打印配置 */
    if (ctx->config.log_level == LOG_LEVEL_DEBUG) {
        config_print(&ctx->config);
    }

    /* === ICMP pinger 初始化 === */
    if (!icmp_pinger_init(&ctx->pinger, ctx->config.use_ipv6, error_msg, error_size)) {
        logger_destroy(&ctx->logger);
        return false;
    }

    /* === 指标初始化 === */
    metrics_init(&ctx->metrics);

#ifdef OPENUPS_HAS_SYSTEMD
    /* === systemd 集成初始化 === */
    if (ctx->config.enable_systemd) {
        systemd_notifier_init(&ctx->systemd);
        ctx->systemd_enabled = systemd_notifier_is_enabled(&ctx->systemd);

        if (ctx->systemd_enabled) {
            logger_debug(&ctx->logger, "systemd integration enabled");

            if (ctx->config.enable_watchdog) {
                ctx->watchdog_interval_ms = systemd_notifier_watchdog_interval_ms(&ctx->systemd);
                logger_debug(&ctx->logger, "watchdog interval: %" PRIu64 "ms",
                             ctx->watchdog_interval_ms);
            }
        } else {
            logger_debug(&ctx->logger, "systemd not detected, integration disabled");
        }
    } else {
        systemd_notifier_destroy(&ctx->systemd);
    }
#endif /* OPENUPS_HAS_SYSTEMD */

    return true;
}

void openups_ctx_destroy(openups_ctx_t* restrict ctx)
{
    if (ctx == NULL) {
        return;
    }

    icmp_pinger_destroy(&ctx->pinger);
#ifdef OPENUPS_HAS_SYSTEMD
    systemd_notifier_destroy(&ctx->systemd);
#endif
    logger_destroy(&ctx->logger);

    g_ctx = NULL;
    memset(ctx, 0, sizeof(*ctx));
}

/* === 统计信息打印 === */

void openups_ctx_print_stats(openups_ctx_t* restrict ctx)
{
    if (ctx == NULL) {
        return;
    }

    const metrics_t* metrics = &ctx->metrics;

    logger_info(&ctx->logger,
                "Statistics: %" PRIu64 " total pings, %" PRIu64 " successful, %" PRIu64
                " failed (%.2f%% success rate), latency min %.2fms / max %.2fms / avg %.2fms, "
                "uptime %" PRIu64 " seconds",
                metrics->total_pings, metrics->successful_pings, metrics->failed_pings,
                metrics_success_rate(metrics), metrics->min_latency, metrics->max_latency,
                metrics_avg_latency(metrics), metrics_uptime_seconds(metrics));
}

/* === Ping 执行（带重试） === */

bool openups_ctx_ping_once(openups_ctx_t* restrict ctx, ping_result_t* restrict result)
{
    if (ctx == NULL || result == NULL) {
        return false;
    }

    const config_t* config = &ctx->config;

    /* 执行 ping（带重试机制） */
    for (int attempt = 0; attempt <= config->max_retries; attempt++) {
        ctx->last_ping_time_ms = get_monotonic_ms();

        *result = icmp_pinger_ping_ex(&ctx->pinger, config->target, config->timeout_ms,
                                      config->payload_size, watchdog_tick_callback, ctx,
                                      monitor_should_stop, ctx);

        if (result->success) {
            return true;
        }

        if (ctx->stop_flag) {
            return false;
        }

        /* 重试前延迟 100ms */
        if (attempt < config->max_retries) {
            struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000L};
            while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
                if (ctx->stop_flag) {
                    break;
                }
            }
        }
    }

    return false;
}

/* === 可中断休眠 === */

void openups_ctx_sleep_interruptible(openups_ctx_t* restrict ctx, int seconds)
{
    if (ctx == NULL || seconds <= 0) {
        return;
    }

#ifdef OPENUPS_HAS_SYSTEMD
    const bool watchdog_enabled = ctx->systemd_enabled && ctx->config.enable_watchdog;
    const uint64_t watchdog_interval_ms = watchdog_enabled ? ctx->watchdog_interval_ms : 0;
#endif

    uint64_t remaining_ms = 0;
    if (ckd_mul(&remaining_ms, (uint64_t)seconds, UINT64_C(1000))) {
        remaining_ms = UINT64_MAX;
    }

    while (remaining_ms > 0) {
        if (OPENUPS_UNLIKELY(ctx->stop_flag)) {
            break;
        }

        /* 计算本次休眠时长（不超过 watchdog 间隔） */
        uint64_t chunk_ms = remaining_ms;
#ifdef OPENUPS_HAS_SYSTEMD
        if (watchdog_enabled && watchdog_interval_ms > 0 && watchdog_interval_ms < chunk_ms) {
            chunk_ms = watchdog_interval_ms;
        }
#endif

        /* 休眠 */
        struct timespec ts = {.tv_sec = (time_t)(chunk_ms / 1000ULL),
                              .tv_nsec = (long)((chunk_ms % 1000ULL) * 1000000ULL)};
        while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
            if (ctx->stop_flag) {
                break;
            }
        }

#ifdef OPENUPS_HAS_SYSTEMD
        /* 喂 watchdog */
        if (watchdog_enabled) {
            (void)systemd_notifier_watchdog(&ctx->systemd);
        }
#endif

        /* 更新剩余时间 */
        if (chunk_ms >= remaining_ms) {
            remaining_ms = 0;
        } else {
            remaining_ms -= chunk_ms;
        }
    }
}

/* === 监控循环核心逻辑 === */

/* 处理 ping 成功 */
static OPENUPS_HOT void handle_ping_success(openups_ctx_t* restrict ctx, const ping_result_t* restrict result)
{
    if (ctx == NULL || result == NULL) {
        return;
    }

    ctx->consecutive_fails = 0;
    metrics_record_success(&ctx->metrics, result->latency_ms);

    logger_debug(&ctx->logger, "Ping successful to %s, latency: %.2fms",
                 ctx->config.target, result->latency_ms);

#ifdef OPENUPS_HAS_SYSTEMD
    /* systemd 状态通知 */
    if (ctx->systemd_enabled) {
        const metrics_t* metrics = &ctx->metrics;
        double success_rate = metrics_success_rate(metrics);
        notify_systemd_status(ctx,
                              "OK: %" PRIu64 "/%" PRIu64 " pings (%.1f%%), latency %.2fms",
                              metrics->successful_pings, metrics->total_pings, success_rate,
                              result->latency_ms);
    }
#endif
}

/* 处理 ping 失败 */
static OPENUPS_COLD void handle_ping_failure(openups_ctx_t* restrict ctx, const ping_result_t* restrict result)
{
    if (ctx == NULL || result == NULL) {
        return;
    }

    ctx->consecutive_fails++;
    metrics_record_failure(&ctx->metrics);

    logger_warn(&ctx->logger, "Ping failed to %s: %s (consecutive failures: %d)",
                ctx->config.target, result->error_msg, ctx->consecutive_fails);

#ifdef OPENUPS_HAS_SYSTEMD
    /* systemd 状态通知 */
    if (ctx->systemd_enabled) {
        notify_systemd_status(ctx, "WARNING: %d consecutive failures, threshold is %d",
                              ctx->consecutive_fails, ctx->config.fail_threshold);
    }
#endif
}

/* 触发关机 */
static OPENUPS_COLD bool trigger_shutdown(openups_ctx_t* restrict ctx)
{
    if (ctx == NULL || ctx->consecutive_fails < ctx->config.fail_threshold) {
        return false;
    }

#ifdef OPENUPS_HAS_SYSTEMD
    const bool use_systemctl_poweroff = ctx->config.enable_systemd && ctx->systemd_enabled;
#else
    const bool use_systemctl_poweroff = false;
#endif
    shutdown_trigger(&ctx->config, &ctx->logger, use_systemctl_poweroff);

    if (ctx->config.shutdown_mode == SHUTDOWN_MODE_LOG_ONLY) {
        ctx->consecutive_fails = 0; /* 重置计数器，继续监控 */
        return false;
    }

    logger_info(&ctx->logger, "Shutdown triggered, exiting monitor loop");
    return true; /* 退出监控循环 */
}

/* 执行单次监控迭代 */
static OPENUPS_HOT bool run_iteration(openups_ctx_t* restrict ctx)
{
    if (ctx == NULL) {
        return false;
    }

    /* 处理统计信息打印请求 */
    if (OPENUPS_UNLIKELY(ctx->print_stats_flag)) {
        openups_ctx_print_stats(ctx);
        ctx->print_stats_flag = 0;
    }

    /* 检查停止标志 */
    if (OPENUPS_UNLIKELY(ctx->stop_flag)) {
        return true;
    }

    /* 执行 ping */
    ping_result_t result;
    bool success = openups_ctx_ping_once(ctx, &result);

    if (OPENUPS_UNLIKELY(ctx->stop_flag)) {
        return true;
    }

    /* 处理结果 */
    if (OPENUPS_LIKELY(success)) {
        handle_ping_success(ctx, &result);
        return false;
    }

    handle_ping_failure(ctx, &result);
    return trigger_shutdown(ctx);
}

/* === 主监控循环 === */

int openups_ctx_run(openups_ctx_t* restrict ctx)
{
    if (ctx == NULL) {
        return -1;
    }

    const config_t* config = &ctx->config;

    /* 设置信号处理 */
    setup_signal_handlers(ctx);

    /* 启动日志 */
    logger_info(&ctx->logger,
                "Starting OpenUPS monitor for target %s, checking every %d seconds, "
                "shutdown after %d consecutive failures (IPv%s)",
                config->target, config->interval_sec, config->fail_threshold,
                config->use_ipv6 ? "6" : "4");

#ifdef OPENUPS_HAS_SYSTEMD
    /* 通知 systemd ready */
    if (ctx->systemd_enabled) {
        (void)systemd_notifier_ready(&ctx->systemd);
        notify_systemd_status(ctx, "Monitoring %s, checking every %ds, threshold %d failures",
                              config->target, config->interval_sec, config->fail_threshold);
    }
#endif

    /* 主循环 */
    while (!ctx->stop_flag) {
        if (run_iteration(ctx)) {
            break;
        }

        openups_ctx_sleep_interruptible(ctx, config->interval_sec);
    }

    /* 优雅关闭 */
    if (ctx->stop_flag) {
        logger_info(&ctx->logger, "Received shutdown signal, stopping gracefully...");
#ifdef OPENUPS_HAS_SYSTEMD
        if (ctx->systemd_enabled) {
            (void)systemd_notifier_stopping(&ctx->systemd);
        }
#endif
    }

    openups_ctx_print_stats(ctx);
    logger_info(&ctx->logger, "OpenUPS monitor stopped");

    return 0;
}
