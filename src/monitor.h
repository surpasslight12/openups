#ifndef OPENUPS_MONITOR_H
#define OPENUPS_MONITOR_H

#include "config.h"
#include "logger.h"
#include "icmp.h"
#include "systemd.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <signal.h>

/* 监控指标 */
typedef struct {
    uint64_t total_pings;
    uint64_t successful_pings;
    uint64_t failed_pings;
    double min_latency;
    double max_latency;
    double total_latency;
    uint64_t start_time;
} metrics_t;

/* 监控器结构 */
typedef struct {
    config_t* config;
    logger_t* logger;
    metrics_t metrics;
    icmp_pinger_t pinger;
    systemd_notifier_t* systemd;
    
    volatile sig_atomic_t stop_flag;
    volatile sig_atomic_t print_stats_flag;
    
    int consecutive_fails;
} monitor_t;

/* 初始化监控器 */
[[nodiscard]] bool monitor_init(monitor_t* restrict monitor, config_t* restrict config, 
                                logger_t* restrict logger, char* restrict error_msg, 
                                size_t error_size);

/* 销毁监控器 */
void monitor_destroy(monitor_t* restrict monitor);

/* 运行监控循环 */
[[nodiscard]] int monitor_run(monitor_t* restrict monitor);

/* 打印统计信息 */
void monitor_print_statistics(monitor_t* restrict monitor);

/* 信号处理 */
void monitor_setup_signals(monitor_t* restrict monitor);

#endif /* OPENUPS_MONITOR_H */
