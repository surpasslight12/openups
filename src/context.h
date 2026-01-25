#ifndef OPENUPS_CONTEXT_H
#define OPENUPS_CONTEXT_H

#include "config.h"
#include "icmp.h"
#include "logger.h"
#include "metrics.h"
#include "systemd.h"
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>

/* 统一上下文结构：持有配置、组件与运行时状态。 */
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

/* === 上下文生命周期管理 === */

/* 初始化上下文；失败时写入 error_msg。 */
[[nodiscard]] bool openups_ctx_init(openups_ctx_t* restrict ctx, int argc,
                                    char** restrict argv, char* restrict error_msg,
                                    size_t error_size);

/* 销毁上下文并释放资源 */
void openups_ctx_destroy(openups_ctx_t* restrict ctx);

/* === 监控循环 === */

/* 运行主监控循环；返回退出码（0=正常）。 */
[[nodiscard]] int openups_ctx_run(openups_ctx_t* restrict ctx);

/* === 辅助函数 === */

/* 打印统计信息（SIGUSR1）。 */
void openups_ctx_print_stats(openups_ctx_t* restrict ctx);

/* 执行单次 ping（带重试）；结果写入 result。 */
[[nodiscard]] bool openups_ctx_ping_once(openups_ctx_t* restrict ctx,
                                         ping_result_t* restrict result);

/* 可中断休眠（支持 watchdog 心跳和信号中断）。 */
void openups_ctx_sleep_interruptible(openups_ctx_t* restrict ctx, int seconds);

#endif /* OPENUPS_CONTEXT_H */
