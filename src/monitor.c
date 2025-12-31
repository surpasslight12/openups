#include "monitor.h"
#include "common.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* 编译时检查 */
static_assert(sizeof(sig_atomic_t) >= sizeof(int), "sig_atomic_t must be at least int size");

/* 全局监控器实例用于信号处理
 * 注意: 此全局变量在信号处理程序中访问，但只有 stop_flag 和 print_stats_flag
 * 是 sig_atomic_t 类型，保证原子性访问。其他字段不在信号处理程序中修改。
 */
static monitor_t* g_monitor = nullptr;

/* 信号处理函数
 * 注意：信号处理程序必须是异步信号安全的。
 * 只能修改 sig_atomic_t 类型的变量，不能调用非安全函数。
 * 日志记录和 systemd 通知会在主循环中处理。
 */
static void signal_handler(int signum) {
    if (g_monitor) {
        if (signum == SIGINT || signum == SIGTERM) {
            g_monitor->stop_flag = 1;
            /* 不要在信号处理程序中调用 logger_info 或 systemd_notifier_stopping
             * 这些函数不是异步信号安全的，会在主循环中处理 */
        } else if (signum == SIGUSR1) {
            g_monitor->print_stats_flag = 1;
        }
    }
}

/* 设置信号处理函数 (SIGINT, SIGTERM, SIGUSR1) */
void monitor_setup_signals(monitor_t* restrict monitor) {
    if (monitor == nullptr) {
        return;
    }
    
    g_monitor = monitor;
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGUSR1, signal_handler);
}

/* 初始化指标
 * 用于统计 ping 操作的成功率、延迟和运行时间
 * 初始化所有计数器为 0，最小/最大延迟为 -1（表示尚未有数据）
 */
static void metrics_init(metrics_t* metrics) {
    metrics->total_pings = 0;
    metrics->successful_pings = 0;
    metrics->failed_pings = 0;
    metrics->min_latency = -1.0;
    metrics->max_latency = -1.0;
    metrics->total_latency = 0.0;
    metrics->start_time = get_timestamp_ms() / 1000;
}

/* 记录成功的 ping 操作
 * 参数: latency - 本次 ping 的延迟（毫秒）
 * 功能: (1) 增加成功计数, (2) 累加总延迟, (3) 更新最小/最大延迟
 */
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

/* 计算成功率 (百分比)
 * 返回值: 0-100 的百分比，无 ping 操作则返回 0.0
 */
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

/* 初始化监控器（创建 ICMP pinger 和 systemd 集成） */
bool monitor_init(monitor_t* restrict monitor, config_t* restrict config, 
                  logger_t* restrict logger, char* restrict error_msg, size_t error_size) {
    if (monitor == nullptr || config == nullptr || logger == nullptr || 
        error_msg == nullptr || error_size == 0) {
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
        monitor->systemd = (systemd_notifier_t*)malloc(sizeof(systemd_notifier_t));
        if (monitor->systemd == nullptr) {
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
        monitor->systemd = nullptr;
    }
    
    /* 设置信号处理 */
    monitor_setup_signals(monitor);
    
    return true;
}

/* 销毁监控器并释放资源 */
void monitor_destroy(monitor_t* restrict monitor) {
    if (monitor == nullptr) {
        return;
    }
    
    icmp_pinger_destroy(&monitor->pinger);
    
    if (monitor->systemd != nullptr) {
        systemd_notifier_destroy(monitor->systemd);
        free(monitor->systemd);
        monitor->systemd = nullptr;
    }
    
    g_monitor = nullptr;
}

/* 打印统计信息（总 ping 数、成功率、延迟值） */
void monitor_print_statistics(monitor_t* restrict monitor) {
    if (monitor == nullptr || monitor->logger == nullptr) {
        return;
    }
    
    logger_info(monitor->logger, 
                "Statistics: %lu total pings, %lu successful, %lu failed (%.2f%% success rate), "
                "latency min %.2fms / max %.2fms / avg %.2fms, uptime %llu seconds",
                monitor->metrics.total_pings,
                monitor->metrics.successful_pings,
                monitor->metrics.failed_pings,
                metrics_success_rate(&monitor->metrics),
                monitor->metrics.min_latency,
                monitor->metrics.max_latency,
                metrics_avg_latency(&monitor->metrics),
                (unsigned long long)metrics_uptime_seconds(&monitor->metrics));
}

/* 执行 ping（带重试机制）
 * 实现细节:
 *   - 尝试 (max_retries + 1) 次
 *   - 失败后等待 100ms 再重试
 *   - 一旦成功立即返回
 */
static bool perform_ping(monitor_t* restrict monitor, ping_result_t* restrict result) {
    if (monitor == nullptr || result == nullptr) {
        return false;
    }
    
    for (int attempt = 0; attempt <= monitor->config->max_retries; attempt++) {
        *result = icmp_pinger_ping(&monitor->pinger,
                                   monitor->config->target,
                                   monitor->config->timeout_ms,
                                   monitor->config->packet_size);
        
        if (result->success) {
            return true;
        }
        
        /* 重试前延迟 100ms，给网络恢复时间 */
        if (attempt < monitor->config->max_retries) {
            usleep(100000);
        }
    }
    
    return false;
}

/* 处理 ping 成功
 * 功能: (1) 重置失败计数, (2) 记录成功指标, (3) 输出日志
 * 设计: 成功后立即重置失败计数，确保故障恢复时及时发现
 */
static void handle_ping_success(monitor_t* monitor, const ping_result_t* result) {
    monitor->consecutive_fails = 0;  /* 网络已恢复 */
    metrics_record_success(&monitor->metrics, result->latency_ms);
    
    /* 仅 DEBUG 级别输出每次 ping 的详细信息 */
    if (monitor->logger->level >= LOG_LEVEL_DEBUG) {
        logger_debug(monitor->logger, "Ping successful to %s, latency: %.2fms",
                    monitor->config->target, result->latency_ms);
    }
}

/* 处理 ping 失败
 * 功能: (1) 增加失败计数, (2) 记录失败统计, (3) 输出警告日志
 * 注意: 连续失败计数达到阈值后会触发关机决策
 */
static void handle_ping_failure(monitor_t* monitor, const ping_result_t* result) {
    monitor->consecutive_fails++;  /* 累加连续失败次数 */
    metrics_record_failure(&monitor->metrics);
    
    logger_warn(monitor->logger, "Ping failed to %s: %s (consecutive failures: %d)",
                monitor->config->target, result->error_msg, monitor->consecutive_fails);
}

/* 触发关机 */
static void trigger_shutdown(monitor_t* monitor) {
    logger_warn(monitor->logger, "Shutdown threshold reached, mode is %s%s",
                shutdown_mode_to_string(monitor->config->shutdown_mode),
                monitor->config->dry_run ? " (dry-run enabled)" : "");
    
    if (monitor->config->dry_run) {
        logger_info(monitor->logger, "[DRY-RUN] Would trigger shutdown in %s mode",
                   shutdown_mode_to_string(monitor->config->shutdown_mode));
        return;
    }
    
    char shutdown_cmd[512] = {0};
    
    /* 检查是否在 systemd 环境中 */
    bool use_systemctl = monitor->config->enable_systemd && 
                         monitor->systemd != nullptr && 
                         systemd_notifier_is_enabled(monitor->systemd);
    
    switch (monitor->config->shutdown_mode) {
        case SHUTDOWN_MODE_IMMEDIATE:
            if (strlen(monitor->config->shutdown_cmd) > 0) {
                snprintf(shutdown_cmd, sizeof(shutdown_cmd), "%s", 
                        monitor->config->shutdown_cmd);
            } else {
                /* 优先使用 systemctl（更优雅的关机方式） */
                if (use_systemctl) {
                    snprintf(shutdown_cmd, sizeof(shutdown_cmd), "systemctl poweroff");
                } else {
                    snprintf(shutdown_cmd, sizeof(shutdown_cmd), "/sbin/shutdown -h now");
                }
            }
            logger_warn(monitor->logger, "Triggering immediate shutdown");
            break;
            
        case SHUTDOWN_MODE_DELAYED:
            if (strlen(monitor->config->shutdown_cmd) > 0) {
                snprintf(shutdown_cmd, sizeof(shutdown_cmd), "%s", 
                        monitor->config->shutdown_cmd);
            } else {
                /* systemctl 不支持延迟参数，使用 shutdown 命令 */
                snprintf(shutdown_cmd, sizeof(shutdown_cmd),
                        "/sbin/shutdown -h +%d", monitor->config->delay_minutes);
            }
            logger_warn(monitor->logger, "Triggering shutdown in %d minutes",
                       monitor->config->delay_minutes);
            break;
            
        case SHUTDOWN_MODE_LOG_ONLY:
            logger_error(monitor->logger, "LOG-ONLY mode: Network connectivity lost, would trigger shutdown");
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
    
    /* 执行关机命令（使用 fork/execv 替代 system 以提高安全性）
     * 优点: 避免 shell 注入、更精确的错误处理、不阻塞信号
     */
    pid_t child_pid = fork();
    if (child_pid < 0) {
        logger_error(monitor->logger, "fork() failed: %s", strerror(errno));
        return;
    }
    
    if (child_pid == 0) {
        /* 子进程：执行命令 */
        /* 关闭不必要的文件描述符（仅保留 stderr 用于日志） */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            close(devnull);
        }
        
        /* 对于 systemctl/shutdown 等标准命令，使用 system() 简化实现
         * 虽然安全性略低，但更易维护，且我们已做路径验证
         */
        int code = system(shutdown_cmd);
        exit(WIFEXITED(code) ? WEXITSTATUS(code) : 127);
    } else {
        /* 父进程：等待子进程完成（设置 5 秒超时）
         * 防止被杀死的系统命令卡住监控进程
         */
        int status;
        time_t start = time(nullptr);
        pid_t result;
        
        do {
            result = waitpid(child_pid, &status, WNOHANG);
            if (result == child_pid) {
                /* 子进程已完成 */
                if (WIFEXITED(status)) {
                    int exit_code = WEXITSTATUS(status);
                    if (exit_code == 0) {
                        logger_info(monitor->logger, "Shutdown command executed successfully");
                    } else {
                        logger_error(monitor->logger, "Shutdown command failed with exit code %d: %s",
                                    exit_code, shutdown_cmd);
                    }
                } else if (WIFSIGNALED(status)) {
                    logger_error(monitor->logger, "Shutdown command terminated by signal %d: %s",
                                WTERMSIG(status), shutdown_cmd);
                }
                return;
            } else if (result < 0) {
                logger_error(monitor->logger, "waitpid() failed: %s", strerror(errno));
                return;
            }
            
            /* 5 秒超时后强制杀死子进程 */
            if (time(nullptr) - start > 5) {
                logger_warn(monitor->logger, "Shutdown command timeout, killing process");
                kill(child_pid, SIGKILL);
                waitpid(child_pid, &status, 0);
                return;
            }
            
            /* 避免繁忙轮询，休眠 100ms */
            usleep(100000);
        } while (1);
    }
}

/* 可中断的休眠（支持信号中断和 watchdog 心跳）
 * 实现: 逐秒休眠，每秒检查 stop_flag 和发送心跳
 * 优点: 快速响应停止信号，同时保持 watchdog 活跃
 */
static void sleep_with_stop(monitor_t* monitor, int seconds) {
    for (int i = 0; i < seconds; i++) {
        if (monitor->stop_flag) {
            break;  /* 快速响应停止信号 */
        }
        
        /* 每秒发送 systemd watchdog 心跳 */
        if (monitor->systemd != nullptr && 
            systemd_notifier_is_enabled(monitor->systemd) &&
            monitor->config->enable_watchdog) {
            (void)systemd_notifier_watchdog(monitor->systemd);
        }
        
        sleep(1);
    }
}

/* 运行监控循环 (不断 ping 目标主机直到接收停止信号) */
int monitor_run(monitor_t* restrict monitor) {
    if (monitor == nullptr || monitor->config == nullptr || monitor->logger == nullptr) {
        return -1;
    }
    
    /* 打印启动信息 */
    logger_info(monitor->logger, 
                "Starting OpenUPS monitor for target %s, checking every %d seconds, "
                "shutdown after %d consecutive failures (IPv%s)",
                monitor->config->target,
                monitor->config->interval_sec,
                monitor->config->fail_threshold,
                monitor->config->use_ipv6 ? "6" : "4");
    
    /* 通知 systemd 就绪 */
    if (monitor->systemd != nullptr && systemd_notifier_is_enabled(monitor->systemd)) {
        (void)systemd_notifier_ready(monitor->systemd);
        char status_msg[256];
        snprintf(status_msg, sizeof(status_msg), 
                "Monitoring %s, checking every %ds, threshold %d failures",
                monitor->config->target,
                monitor->config->interval_sec,
                monitor->config->fail_threshold);
        (void)systemd_notifier_status(monitor->systemd, status_msg);
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
            if (monitor->systemd != nullptr && systemd_notifier_is_enabled(monitor->systemd)) {
                char status_msg[256];
                double success_rate = metrics_success_rate(&monitor->metrics);
                snprintf(status_msg, sizeof(status_msg),
                        "OK: %lu/%lu pings (%.1f%%), latency %.2fms",
                        monitor->metrics.successful_pings,
                        monitor->metrics.total_pings,
                        success_rate,
                        result.latency_ms);
                (void)systemd_notifier_status(monitor->systemd, status_msg);
            }
        } else {
            handle_ping_failure(monitor, &result);
            
            /* 更新 systemd 状态 - 显示失败警告 */
            if (monitor->systemd != nullptr && systemd_notifier_is_enabled(monitor->systemd)) {
                char status_msg[256];
                snprintf(status_msg, sizeof(status_msg),
                        "WARNING: %d consecutive failures, threshold is %d",
                        monitor->consecutive_fails,
                        monitor->config->fail_threshold);
                (void)systemd_notifier_status(monitor->systemd, status_msg);
            }
            
            /* 检查是否达到失败阈值 */
            if (monitor->consecutive_fails >= monitor->config->fail_threshold) {
                /* LOG_ONLY 模式：仅记录日志并继续监控 */
                if (monitor->config->shutdown_mode == SHUTDOWN_MODE_LOG_ONLY) {
                    trigger_shutdown(monitor);
                    /* 重置计数继续监控 */
                    monitor->consecutive_fails = 0;
                } else {
                    /* 其他模式：触发关机并退出监控循环 */
                    trigger_shutdown(monitor);
                    logger_info(monitor->logger, "Shutdown triggered, exiting monitor loop");
                    break;
                }
            }
        }
        
        /* 休眠（带 watchdog 心跳） */
        sleep_with_stop(monitor, monitor->config->interval_sec);
    }
    
    /* 处理停止信号（在循环外记录日志是安全的） */
    if (monitor->stop_flag) {
        logger_info(monitor->logger, "Received shutdown signal, stopping gracefully...");
        
        /* 通知 systemd 正在停止 */
        if (monitor->systemd != nullptr && systemd_notifier_is_enabled(monitor->systemd)) {
            (void)systemd_notifier_stopping(monitor->systemd);
        }
    }
    
    /* 打印最终统计 */
    monitor_print_statistics(monitor);
    logger_info(monitor->logger, "OpenUPS monitor stopped");
    
    return 0;
}

/* 向监控器发送一个停止信号 */
void monitor_stop(monitor_t* restrict monitor) {
    if (monitor != nullptr) {
        monitor->stop_flag = 1;
    }
}
