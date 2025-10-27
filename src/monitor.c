#include "monitor.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/time.h>

/* 全局监控器实例用于信号处理 */
static monitor_t* g_monitor = NULL;

/* 信号处理函数 */
static void signal_handler(int signum) {
    if (g_monitor) {
        if (signum == SIGINT || signum == SIGTERM) {
            g_monitor->stop_flag = 1;
            logger_info(g_monitor->logger, "Received signal %d, shutting down...", signum);
            
            if (g_monitor->systemd && systemd_notifier_is_enabled(g_monitor->systemd)) {
                systemd_notifier_stopping(g_monitor->systemd);
            }
        } else if (signum == SIGUSR1) {
            g_monitor->print_stats_flag = 1;
        }
    }
}

void monitor_setup_signals(monitor_t* monitor) {
    g_monitor = monitor;
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGUSR1, signal_handler);
}

/* 初始化指标 */
static void metrics_init(metrics_t* metrics) {
    metrics->total_pings = 0;
    metrics->successful_pings = 0;
    metrics->failed_pings = 0;
    metrics->min_latency = -1.0;
    metrics->max_latency = -1.0;
    metrics->total_latency = 0.0;
    metrics->start_time = get_timestamp_ms() / 1000;
}

/* 记录成功 */
static void metrics_record_success(metrics_t* metrics, double latency) {
    metrics->total_pings++;
    metrics->successful_pings++;
    metrics->total_latency += latency;
    
    if (metrics->min_latency < 0 || latency < metrics->min_latency) {
        metrics->min_latency = latency;
    }
    
    if (latency > metrics->max_latency) {
        metrics->max_latency = latency;
    }
}

/* 记录失败 */
static void metrics_record_failure(metrics_t* metrics) {
    metrics->total_pings++;
    metrics->failed_pings++;
}

/* 计算成功率 */
static double metrics_success_rate(const metrics_t* metrics) {
    if (metrics->total_pings == 0) {
        return 0.0;
    }
    return (double)metrics->successful_pings / metrics->total_pings * 100.0;
}

/* 计算平均延迟 */
static double metrics_avg_latency(const metrics_t* metrics) {
    if (metrics->successful_pings == 0) {
        return 0.0;
    }
    return metrics->total_latency / metrics->successful_pings;
}

/* 计算运行时间 */
static uint64_t metrics_uptime_seconds(const metrics_t* metrics) {
    uint64_t now = get_timestamp_ms() / 1000;
    return now - metrics->start_time;
}

bool monitor_init(monitor_t* monitor, config_t* config, logger_t* logger,
                  char* error_msg, size_t error_size) {
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
        monitor->systemd = (systemd_notifier_t*)malloc(sizeof(systemd_notifier_t));
        if (!monitor->systemd) {
            snprintf(error_msg, error_size, "Failed to allocate systemd notifier");
            icmp_pinger_destroy(&monitor->pinger);
            return false;
        }
        systemd_notifier_init(monitor->systemd);
        if (systemd_notifier_is_enabled(monitor->systemd)) {
            logger_debug(logger, "systemd integration enabled");
        } else {
            logger_debug(logger, "systemd not detected, integration disabled");
        }
    } else {
        monitor->systemd = NULL;
    }
    
    /* 设置信号处理 */
    monitor_setup_signals(monitor);
    
    return true;
}

void monitor_destroy(monitor_t* monitor) {
    icmp_pinger_destroy(&monitor->pinger);
    
    if (monitor->systemd) {
        systemd_notifier_destroy(monitor->systemd);
        free(monitor->systemd);
        monitor->systemd = NULL;
    }
    
    g_monitor = NULL;
}

void monitor_print_statistics(monitor_t* monitor) {
    logger_info(monitor->logger, 
                "Statistics: total=%lu successful=%lu failed=%lu success_rate=%.2f%% "
                "min_latency=%.2fms max_latency=%.2fms avg_latency=%.2fms uptime=%lus",
                monitor->metrics.total_pings,
                monitor->metrics.successful_pings,
                monitor->metrics.failed_pings,
                metrics_success_rate(&monitor->metrics),
                monitor->metrics.min_latency,
                monitor->metrics.max_latency,
                metrics_avg_latency(&monitor->metrics),
                metrics_uptime_seconds(&monitor->metrics));
}

/* 执行 ping（带重试） */
static bool perform_ping(monitor_t* monitor, ping_result_t* result) {
    for (int attempt = 0; attempt <= monitor->config->max_retries; attempt++) {
        *result = icmp_pinger_ping(&monitor->pinger,
                                   monitor->config->target,
                                   monitor->config->timeout_ms,
                                   monitor->config->packet_size);
        
        if (result->success) {
            return true;
        }
        
        /* 重试前短暂延迟 */
        if (attempt < monitor->config->max_retries) {
            usleep(100000);  /* 100ms */
        }
    }
    
    return false;
}

/* 处理 ping 成功 */
static void handle_ping_success(monitor_t* monitor, const ping_result_t* result) {
    monitor->consecutive_fails = 0;
    metrics_record_success(&monitor->metrics, result->latency_ms);
    
    /* DEBUG 级别才输出每次成功的 ping 详细信息 */
    if (monitor->logger->level >= LOG_LEVEL_DEBUG) {
        logger_debug(monitor->logger, "Ping successful to %s, latency: %.2fms",
                    monitor->config->target, result->latency_ms);
    }
}

/* 处理 ping 失败 */
static void handle_ping_failure(monitor_t* monitor, const ping_result_t* result) {
    monitor->consecutive_fails++;
    metrics_record_failure(&monitor->metrics);
    
    logger_warn(monitor->logger, "Ping failed to %s: %s (consecutive failures: %d)",
                monitor->config->target, result->error_msg, monitor->consecutive_fails);
}

/* 触发关机 */
static void trigger_shutdown(monitor_t* monitor) {
    logger_warn(monitor->logger, "Shutdown threshold reached (mode: %s, dry_run: %s)",
                shutdown_mode_to_string(monitor->config->shutdown_mode),
                monitor->config->dry_run ? "true" : "false");
    
    if (monitor->config->dry_run) {
        logger_info(monitor->logger, "[DRY-RUN] Would trigger shutdown: mode=%s",
                   shutdown_mode_to_string(monitor->config->shutdown_mode));
        return;
    }
    
    char shutdown_cmd[512] = {0};
    
    switch (monitor->config->shutdown_mode) {
        case SHUTDOWN_MODE_IMMEDIATE:
            if (strlen(monitor->config->shutdown_cmd) > 0) {
                snprintf(shutdown_cmd, sizeof(shutdown_cmd), "%s", 
                        monitor->config->shutdown_cmd);
            } else {
                snprintf(shutdown_cmd, sizeof(shutdown_cmd), "/sbin/shutdown -h now");
            }
            logger_warn(monitor->logger, "Triggering immediate shutdown");
            break;
            
        case SHUTDOWN_MODE_DELAYED:
            if (strlen(monitor->config->shutdown_cmd) > 0) {
                snprintf(shutdown_cmd, sizeof(shutdown_cmd), "%s", 
                        monitor->config->shutdown_cmd);
            } else {
                snprintf(shutdown_cmd, sizeof(shutdown_cmd),
                        "/sbin/shutdown -h +%d", monitor->config->delay_minutes);
            }
            logger_warn(monitor->logger, "Triggering delayed shutdown (%d minutes)",
                       monitor->config->delay_minutes);
            break;
            
        case SHUTDOWN_MODE_LOG_ONLY:
            logger_error(monitor->logger, "LOG-ONLY mode: Would shutdown but only logging");
            return;
            
        case SHUTDOWN_MODE_CUSTOM:
            if (strlen(monitor->config->custom_script) == 0) {
                logger_error(monitor->logger, "Custom script not specified");
                return;
            }
            /* 验证脚本路径安全性 */
            if (!is_safe_path(monitor->config->custom_script)) {
                logger_error(monitor->logger, "Custom script path contains unsafe characters");
                return;
            }
            snprintf(shutdown_cmd, sizeof(shutdown_cmd), "%s", 
                    monitor->config->custom_script);
            logger_warn(monitor->logger, "Executing custom script: %s", shutdown_cmd);
            break;
    }
    
    /* 执行关机命令 */
    int code = system(shutdown_cmd);
    if (code != 0) {
        logger_error(monitor->logger, "Shutdown command failed (exit code: %d, command: %s)",
                    code, shutdown_cmd);
    } else {
        logger_info(monitor->logger, "Shutdown command executed successfully");
    }
}

/* 可中断的休眠（发送 watchdog 心跳） */
static void sleep_with_stop(monitor_t* monitor, int seconds) {
    for (int i = 0; i < seconds; i++) {
        if (monitor->stop_flag) {
            break;
        }
        
        /* 发送 watchdog 心跳 */
        if (monitor->systemd && 
            systemd_notifier_is_enabled(monitor->systemd) &&
            monitor->config->enable_watchdog) {
            systemd_notifier_watchdog(monitor->systemd);
        }
        
        sleep(1);
    }
}

int monitor_run(monitor_t* monitor) {
    /* 打印启动信息 */
    logger_info(monitor->logger, 
                "Starting OpenUPS monitor: target=%s interval=%ds threshold=%d ipv6=%s",
                monitor->config->target,
                monitor->config->interval_sec,
                monitor->config->fail_threshold,
                monitor->config->use_ipv6 ? "true" : "false");
    
    /* 通知 systemd 就绪 */
    if (monitor->systemd && systemd_notifier_is_enabled(monitor->systemd)) {
        systemd_notifier_ready(monitor->systemd);
        char status_msg[256];
        snprintf(status_msg, sizeof(status_msg), 
                "Monitoring %s (interval=%ds, threshold=%d)",
                monitor->config->target,
                monitor->config->interval_sec,
                monitor->config->fail_threshold);
        systemd_notifier_status(monitor->systemd, status_msg);
    }
    
    /* 主监控循环 */
    while (!monitor->stop_flag) {
        /* 检查是否需要打印统计 */
        if (monitor->print_stats_flag) {
            monitor_print_statistics(monitor);
            monitor->print_stats_flag = 0;
        }
        
        /* 执行 ping */
        ping_result_t result;
        bool success = perform_ping(monitor, &result);
        
        if (success) {
            handle_ping_success(monitor, &result);
            
            /* 更新 systemd 状态 - 显示成功统计 */
            if (monitor->systemd && systemd_notifier_is_enabled(monitor->systemd)) {
                char status_msg[256];
                double success_rate = metrics_success_rate(&monitor->metrics);
                snprintf(status_msg, sizeof(status_msg),
                        "OK: %lu/%lu pings (%.1f%%), latency %.2fms",
                        monitor->metrics.successful_pings,
                        monitor->metrics.total_pings,
                        success_rate,
                        result.latency_ms);
                systemd_notifier_status(monitor->systemd, status_msg);
            }
        } else {
            handle_ping_failure(monitor, &result);
            
            /* 更新 systemd 状态 - 显示失败警告 */
            if (monitor->systemd && systemd_notifier_is_enabled(monitor->systemd)) {
                char status_msg[256];
                snprintf(status_msg, sizeof(status_msg),
                        "WARNING: %d consecutive failures (threshold: %d)",
                        monitor->consecutive_fails,
                        monitor->config->fail_threshold);
                systemd_notifier_status(monitor->systemd, status_msg);
            }
            
            /* 检查是否达到失败阈值 */
            if (monitor->consecutive_fails >= monitor->config->fail_threshold) {
                /* LOG_ONLY 模式：仅记录日志并继续监控 */
                if (monitor->config->shutdown_mode == SHUTDOWN_MODE_LOG_ONLY) {
                    trigger_shutdown(monitor);
                    monitor->consecutive_fails = 0;  /* 重置计数继续监控 */
                } else {
                    /* 其他模式：触发关机并退出监控循环 */
                    trigger_shutdown(monitor);
                    logger_info(monitor->logger, "Shutdown triggered, exiting monitor loop");
                    break;
                }
            }
        }
        
        if (monitor->stop_flag) {
            break;
        }
        
        /* 休眠（带 watchdog 心跳） */
        sleep_with_stop(monitor, monitor->config->interval_sec);
    }
    
    /* 打印最终统计 */
    monitor_print_statistics(monitor);
    logger_info(monitor->logger, "OpenUPS monitor stopped");
    
    return 0;
}

void monitor_stop(monitor_t* monitor) {
    monitor->stop_flag = 1;
}
