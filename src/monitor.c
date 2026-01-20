#include "monitor.h"
#include "common.h"
#include "metrics.h"
#include "shutdown.h"
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* 编译时检查 */
static_assert(sizeof(sig_atomic_t) >= sizeof(int), "sig_atomic_t must be at least int size");

/* 全局监控器实例用于信号处理
 * 注意: 此全局变量在信号处理程序中访问，但只有 stop_flag 和 print_stats_flag
 * 是 sig_atomic_t 类型，保证原子性访问。其他字段不在信号处理程序中修改。
 */
static monitor_t* g_monitor = NULL;

/* 信号处理函数
 * 注意：信号处理程序必须是异步信号安全的。
 * 只能修改 sig_atomic_t 类型的变量，不能调用非安全函数。
 * 日志记录和 systemd 通知会在主循环中处理。
 */
static void signal_handler(int signum)
{
    if (g_monitor == NULL) {
        return;
    }

    if (signum == SIGINT || signum == SIGTERM) {
        g_monitor->stop_flag = 1;
    } else if (signum == SIGUSR1) {
        g_monitor->print_stats_flag = 1;
    }
}

/* 设置信号处理函数 (SIGINT, SIGTERM, SIGUSR1) */
void monitor_setup_signals(monitor_t* restrict monitor)
{
    if (monitor == NULL) {
        return;
    }

    g_monitor = monitor;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);

    (void)sigaction(SIGINT, &sa, NULL);
    (void)sigaction(SIGTERM, &sa, NULL);
    (void)sigaction(SIGUSR1, &sa, NULL);
}

/* metrics_* moved to metrics.c */

/* 判断 systemd 是否可用 */
static bool monitor_systemd_enabled(const monitor_t* monitor)
{
    return monitor != NULL && systemd_notifier_is_enabled(&monitor->systemd);
}

static void monitor_icmp_tick(void* user_data)
{
    monitor_t* monitor = (monitor_t*)user_data;
    if (monitor == NULL) {
        return;
    }

    if (monitor_systemd_enabled(monitor) && monitor->config->enable_watchdog) {
        (void)systemd_notifier_watchdog(&monitor->systemd);
    }
}

static bool monitor_icmp_should_stop(void* user_data)
{
    monitor_t* monitor = (monitor_t*)user_data;
    if (monitor == NULL) {
        return true;
    }
    return monitor->stop_flag != 0;
}

/* 初始化监控器（创建 ICMP pinger 和 systemd 集成） */
bool monitor_init(monitor_t* restrict monitor, config_t* restrict config, logger_t* restrict logger,
                  char* restrict error_msg, size_t error_size)
{
    if (monitor == NULL || config == NULL || logger == NULL || error_msg == NULL ||
        error_size == 0) {
        return false;
    }

    monitor->config = config;
    monitor->logger = logger;
    monitor->stop_flag = 0;
    monitor->print_stats_flag = 0;
    monitor->consecutive_fails = 0;

    /* 初始化指标 */
    metrics_init(&monitor->metrics);

    /* 初始化 ICMP pinger */
    if (!icmp_pinger_init(&monitor->pinger, config->use_ipv6, error_msg, error_size)) {
        return false;
    }

    /* 初始化 systemd 通知器 */
    if (config->enable_systemd) {
        systemd_notifier_init(&monitor->systemd);
        if (systemd_notifier_is_enabled(&monitor->systemd)) {
            logger_debug(logger, "systemd integration enabled");
        } else {
            logger_debug(logger, "systemd not detected, integration disabled");
        }
    } else {
        systemd_notifier_destroy(&monitor->systemd);
    }

    /* 设置信号处理 */
    monitor_setup_signals(monitor);

    return true;
}

/* 销毁监控器并释放资源 */
void monitor_destroy(monitor_t* restrict monitor)
{
    if (monitor == NULL) {
        return;
    }

    icmp_pinger_destroy(&monitor->pinger);

    systemd_notifier_destroy(&monitor->systemd);

    g_monitor = NULL;
}

/* 打印统计信息（总 ping 数、成功率、延迟值） */
void monitor_print_statistics(monitor_t* restrict monitor)
{
    if (monitor == NULL || monitor->logger == NULL) {
        return;
    }

    logger_info(monitor->logger,
                "Statistics: %" PRIu64 " total pings, %" PRIu64 " successful, %" PRIu64
                " failed (%.2f%% success rate), latency min %.2fms / max %.2fms / avg %.2fms, "
                "uptime %" PRIu64 " seconds",
                monitor->metrics.total_pings, monitor->metrics.successful_pings,
                monitor->metrics.failed_pings, metrics_success_rate(&monitor->metrics),
                monitor->metrics.min_latency, monitor->metrics.max_latency,
                metrics_avg_latency(&monitor->metrics), metrics_uptime_seconds(&monitor->metrics));
}

/* 执行 ping（带重试机制） */
static bool perform_ping(monitor_t* restrict monitor, ping_result_t* restrict result)
{
    if (monitor == NULL || result == NULL) {
        return false;
    }

    for (int attempt = 0; attempt <= monitor->config->max_retries; attempt++) {
        *result = icmp_pinger_ping_ex(&monitor->pinger, monitor->config->target,
                                      monitor->config->timeout_ms,
                                      monitor->config->packet_size, monitor_icmp_tick, monitor,
                                      monitor_icmp_should_stop, monitor);

        if (result->success) {
            return true;
        }

        /* 重试前延迟 100ms，给网络恢复时间 */
        if (attempt < monitor->config->max_retries) {
            struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000L};
            while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
                if (monitor->stop_flag) {
                    break;
                }
            }
        }
    }

    return false;
}

/* 处理 ping 成功 */
static void handle_ping_success(monitor_t* monitor, const ping_result_t* result)
{
    monitor->consecutive_fails = 0;
    metrics_record_success(&monitor->metrics, result->latency_ms);

    if (monitor->logger->level >= LOG_LEVEL_DEBUG) {
        logger_debug(monitor->logger, "Ping successful to %s, latency: %.2fms",
                     monitor->config->target, result->latency_ms);
    }
}

/* 处理 ping 失败 */
static void handle_ping_failure(monitor_t* monitor, const ping_result_t* result)
{
    monitor->consecutive_fails++;
    metrics_record_failure(&monitor->metrics);

    logger_warn(monitor->logger, "Ping failed to %s: %s (consecutive failures: %d)",
                monitor->config->target, result->error_msg, monitor->consecutive_fails);
}

/* 构建命令参数数组（空白分隔，不支持引号） */
static void sleep_with_stop(monitor_t* monitor, int seconds);

/* 可中断的休眠（支持信号中断和 watchdog 心跳） */
static void sleep_with_stop(monitor_t* monitor, int seconds)
{
    if (monitor == NULL || seconds <= 0) {
        return;
    }

    uint64_t remaining_ms = 0;
    if (ckd_mul(&remaining_ms, (uint64_t)seconds, UINT64_C(1000))) {
        remaining_ms = UINT64_MAX;
    }

    while (remaining_ms > 0) {
        if (monitor->stop_flag) {
            break;
        }

        uint64_t chunk_ms = remaining_ms;
        if (monitor_systemd_enabled(monitor) && monitor->config->enable_watchdog) {
            uint64_t interval_ms = systemd_notifier_watchdog_interval_ms(&monitor->systemd);
            if (interval_ms > 0 && interval_ms < chunk_ms) {
                chunk_ms = interval_ms;
            }
            (void)systemd_notifier_watchdog(&monitor->systemd);
        }

        struct timespec ts = {.tv_sec = (time_t)(chunk_ms / 1000ULL),
                              .tv_nsec = (long)((chunk_ms % 1000ULL) * 1000000ULL)};
        while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
            if (monitor->stop_flag) {
                break;
            }
        }

        if (chunk_ms >= remaining_ms) {
            remaining_ms = 0;
        } else {
            remaining_ms -= chunk_ms;
        }
    }
}

/* 运行监控循环 (不断 ping 目标主机直到接收停止信号) */
int monitor_run(monitor_t* restrict monitor)
{
    if (monitor == NULL || monitor->config == NULL || monitor->logger == NULL) {
        return -1;
    }

    logger_info(monitor->logger,
                "Starting OpenUPS monitor for target %s, checking every %d seconds, "
                "shutdown after %d consecutive failures (IPv%s)",
                monitor->config->target, monitor->config->interval_sec,
                monitor->config->fail_threshold, monitor->config->use_ipv6 ? "6" : "4");

    if (monitor_systemd_enabled(monitor)) {
        (void)systemd_notifier_ready(&monitor->systemd);
        char status_msg[256];
        snprintf(status_msg, sizeof(status_msg),
                 "Monitoring %s, checking every %ds, threshold %d failures",
                 monitor->config->target, monitor->config->interval_sec,
                 monitor->config->fail_threshold);
        (void)systemd_notifier_status(&monitor->systemd, status_msg);
    }

    while (!monitor->stop_flag) {
        if (monitor->print_stats_flag) {
            monitor_print_statistics(monitor);
            monitor->print_stats_flag = 0;
        }

        ping_result_t result;
        bool success = perform_ping(monitor, &result);

        if (success) {
            handle_ping_success(monitor, &result);

            if (monitor_systemd_enabled(monitor)) {
                char status_msg[256];
                double success_rate = metrics_success_rate(&monitor->metrics);
                snprintf(status_msg, sizeof(status_msg),
                         "OK: %" PRIu64 "/%" PRIu64 " pings (%.1f%%), latency %.2fms",
                         monitor->metrics.successful_pings, monitor->metrics.total_pings,
                         success_rate, result.latency_ms);
                (void)systemd_notifier_status(&monitor->systemd, status_msg);
            }
        } else {
            handle_ping_failure(monitor, &result);

            if (monitor_systemd_enabled(monitor)) {
                char status_msg[256];
                snprintf(status_msg, sizeof(status_msg),
                         "WARNING: %d consecutive failures, threshold is %d",
                         monitor->consecutive_fails, monitor->config->fail_threshold);
                (void)systemd_notifier_status(&monitor->systemd, status_msg);
            }

            if (monitor->consecutive_fails >= monitor->config->fail_threshold) {
                if (monitor->config->shutdown_mode == SHUTDOWN_MODE_LOG_ONLY) {
                    bool use_systemctl = monitor->config->enable_systemd &&
                                         monitor_systemd_enabled(monitor);
                    shutdown_trigger(monitor->config, monitor->logger, use_systemctl);
                    monitor->consecutive_fails = 0;
                } else {
                    bool use_systemctl = monitor->config->enable_systemd &&
                                         monitor_systemd_enabled(monitor);
                    shutdown_trigger(monitor->config, monitor->logger, use_systemctl);
                    logger_info(monitor->logger, "Shutdown triggered, exiting monitor loop");
                    break;
                }
            }
        }

        sleep_with_stop(monitor, monitor->config->interval_sec);
    }

    if (monitor->stop_flag) {
        logger_info(monitor->logger, "Received shutdown signal, stopping gracefully...");
        if (monitor_systemd_enabled(monitor)) {
            (void)systemd_notifier_stopping(&monitor->systemd);
        }
    }

    monitor_print_statistics(monitor);
    logger_info(monitor->logger, "OpenUPS monitor stopped");

    return 0;
}
