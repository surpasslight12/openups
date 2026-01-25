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

/* 统一上下文结构 - 全局状态管理
 * 设计原则：
 *   1. 单一实例传递，避免多参数传递
 *   2. 所有模块通过 ctx 访问配置和状态
 *   3. 内存布局优化：热数据靠前（cache-friendly）
 */
typedef struct openups_context {
    /* === 热路径数据（频繁访问） === */
    volatile sig_atomic_t stop_flag;        /* 停止标志（信号安全） */
    volatile sig_atomic_t print_stats_flag; /* 打印统计标志 */
    int consecutive_fails;                  /* 连续失败计数 */

    /* === 核心组件 === */
    config_t config;           /* 配置（栈上，避免指针跳转） */
    logger_t logger;           /* 日志器 */
    icmp_pinger_t pinger;      /* ICMP ping 器 */
    systemd_notifier_t systemd; /* systemd 通知器 */
    metrics_t metrics;         /* 统计指标 */

    /* === 状态标志 === */
    bool systemd_enabled;         /* systemd 是否启用 */
    uint64_t watchdog_interval_ms; /* watchdog 心跳间隔 */

    /* === 性能优化缓存 === */
    uint64_t last_ping_time_ms;  /* 上次 ping 时间（避免重复 clock_gettime） */
    uint64_t start_time_ms;      /* 启动时间（用于 uptime 计算） */
} openups_ctx_t;

/* === 上下文生命周期管理 === */

/* 初始化上下文
 * 职责：
 *   1. 零初始化
 *   2. 加载配置（默认 -> 环境变量 -> 命令行）
 *   3. 验证配置
 *   4. 初始化所有组件（logger, pinger, systemd, metrics）
 * 返回：成功返回 true，失败返回 false 并设置 error_msg
 */
[[nodiscard]] bool openups_ctx_init(openups_ctx_t* restrict ctx, int argc,
                                    char** restrict argv, char* restrict error_msg,
                                    size_t error_size);

/* 销毁上下文并释放资源 */
void openups_ctx_destroy(openups_ctx_t* restrict ctx);

/* === 监控循环 === */

/* 运行主监控循环
 * 职责：
 *   1. 设置信号处理
 *   2. 通知 systemd ready
 *   3. 循环执行 ping + 失败处理
 *   4. 处理停止信号
 * 返回：退出码（0=正常，非0=异常）
 */
[[nodiscard]] int openups_ctx_run(openups_ctx_t* restrict ctx);

/* === 辅助函数 === */

/* 打印统计信息（响应 SIGUSR1） */
void openups_ctx_print_stats(openups_ctx_t* restrict ctx);

/* 执行单次 ping（带重试机制）
 * 返回：true=成功，false=失败
 */
[[nodiscard]] bool openups_ctx_ping_once(openups_ctx_t* restrict ctx,
                                         ping_result_t* restrict result);

/* 可中断休眠（支持 watchdog 心跳和信号中断）
 * 内部优化：
 *   - 分块休眠，每块不超过 watchdog 间隔
 *   - 周期性检查 stop_flag
 */
void openups_ctx_sleep_interruptible(openups_ctx_t* restrict ctx, int seconds);

#endif /* OPENUPS_CONTEXT_H */
