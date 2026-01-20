#include "monitor.h"
#include "common.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
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

/* 初始化指标
 * 用于统计 ping 操作的成功率、延迟和运行时间
 * 初始化所有计数器为 0，最小/最大延迟为 -1（表示尚未有数据）
 */
static void metrics_init(metrics_t* metrics)
{
    metrics->total_pings = 0;
    metrics->successful_pings = 0;
    metrics->failed_pings = 0;
    metrics->min_latency = -1.0;
    metrics->max_latency = -1.0;
    metrics->total_latency = 0.0;
    metrics->start_time_ms = get_monotonic_ms();
}

/* 记录成功的 ping 操作 */
static void metrics_record_success(metrics_t* metrics, double latency)
{
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
static void metrics_record_failure(metrics_t* metrics)
{
    metrics->total_pings++;
    metrics->failed_pings++;
}

/* 计算成功率 (百分比) */
static double metrics_success_rate(const metrics_t* metrics)
{
    if (metrics->total_pings == 0) {
        return 0.0;
    }
    return (double)metrics->successful_pings / metrics->total_pings * 100.0;
}

/* 计算平均延迟 */
static double metrics_avg_latency(const metrics_t* metrics)
{
    if (metrics->successful_pings == 0) {
        return 0.0;
    }
    return metrics->total_latency / metrics->successful_pings;
}

/* 计算运行时间 */
static uint64_t metrics_uptime_seconds(const metrics_t* metrics)
{
    uint64_t now_ms = get_monotonic_ms();
    if (now_ms == UINT64_MAX || metrics->start_time_ms == UINT64_MAX ||
        now_ms < metrics->start_time_ms) {
        return 0;
    }
    return (now_ms - metrics->start_time_ms) / 1000;
}

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
static bool build_command_argv(const char* command, char* buffer, size_t buffer_size, char* argv[],
                               size_t argv_size)
{
    if (command == NULL || buffer == NULL || argv == NULL || argv_size < 2) {
        return false;
    }

    size_t cmd_len = strlen(command);
    if (cmd_len == 0 || cmd_len >= buffer_size) {
        return false;
    }

    /* Strictly reject quotes/backticks and control characters */
    if (strchr(command, '\"') != NULL || strchr(command, '\'') != NULL ||
        strchr(command, '`') != NULL) {
        return false;
    }

    for (const char* p = command; *p != '\0'; ++p) {
        if ((unsigned char)*p < 0x20 || *p == 0x7F) {
            return false;
        }
    }

    memcpy(buffer, command, cmd_len + 1);

    size_t argc = 0;
    char* saveptr = NULL;
    char* token = strtok_r(buffer, " \t", &saveptr);
    while (token != NULL) {
        if (argc + 1 >= argv_size) {
            return false;
        }
        if (!is_safe_path(token)) {
            return false;
        }
        argv[argc++] = token;
        token = strtok_r(NULL, " \t", &saveptr);
    }

    if (argc == 0) {
        return false;
    }

    argv[argc] = NULL;
    return true;
}

static void sleep_with_stop(monitor_t* monitor, int seconds);

/* 触发关机 */
static void trigger_shutdown(monitor_t* monitor)
{
    logger_warn(monitor->logger, "Shutdown threshold reached, mode is %s%s",
                shutdown_mode_to_string(monitor->config->shutdown_mode),
                monitor->config->dry_run ? " (dry-run enabled)" : "");

    if (monitor->config->dry_run) {
        logger_info(monitor->logger, "[DRY-RUN] Would trigger shutdown in %s mode",
                    shutdown_mode_to_string(monitor->config->shutdown_mode));
        return;
    }

    bool use_systemctl = monitor->config->enable_systemd && monitor_systemd_enabled(monitor);
    char command_buf[512];
    char* argv[16] = {0};

    if (monitor->config->shutdown_mode == SHUTDOWN_MODE_LOG_ONLY) {
        logger_error(monitor->logger,
                     "LOG-ONLY mode: Network connectivity lost, would trigger shutdown");
        return;
    }

    /* Default to userspace shutdown paths (no direct reboot syscall) */
    if (monitor->config->shutdown_mode == SHUTDOWN_MODE_IMMEDIATE) {
        if (use_systemctl) {
            snprintf(command_buf, sizeof(command_buf), "systemctl poweroff");
        } else {
            snprintf(command_buf, sizeof(command_buf), "/sbin/shutdown -h now");
        }
    } else if (monitor->config->shutdown_mode == SHUTDOWN_MODE_DELAYED) {
        snprintf(command_buf, sizeof(command_buf), "/sbin/shutdown -h +%d",
                 monitor->config->delay_minutes);
    } else {
        logger_error(monitor->logger, "Unknown shutdown mode");
        return;
    }

    if (!build_command_argv(command_buf, command_buf, sizeof(command_buf), argv,
                            sizeof(argv) / sizeof(argv[0]))) {
        logger_error(monitor->logger, "Failed to parse shutdown command: %s", command_buf);
        return;
    }

    if (monitor->config->shutdown_mode == SHUTDOWN_MODE_IMMEDIATE) {
        logger_warn(monitor->logger, "Triggering immediate shutdown");
    } else {
        logger_warn(monitor->logger, "Triggering shutdown in %d minutes",
                    monitor->config->delay_minutes);
    }

    /* 执行关机命令（使用 fork/execvp 以避免 shell 注入） */
    pid_t child_pid = fork();
    if (child_pid < 0) {
        logger_error(monitor->logger, "fork() failed: %s", strerror(errno));
        return;
    }

    if (child_pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            (void)dup2(devnull, STDOUT_FILENO);
            close(devnull);
        }

        execvp(argv[0], argv);

        fprintf(stderr, "exec failed: %s\n", strerror(errno));
        _exit(127);
    }

    /* 父进程：等待子进程完成（设置 5 秒超时） */
    int status = 0;
    time_t start_time = time(NULL);
    while (1) {
        pid_t result = waitpid(child_pid, &status, WNOHANG);
        if (result == child_pid) {
            if (WIFEXITED(status)) {
                int exit_code = WEXITSTATUS(status);
                if (exit_code == 0) {
                    logger_info(monitor->logger, "Shutdown command executed successfully");
                } else {
                    logger_error(monitor->logger, "Shutdown command failed with exit code %d: %s",
                                 exit_code, argv[0]);
                }
            } else if (WIFSIGNALED(status)) {
                logger_error(monitor->logger, "Shutdown command terminated by signal %d: %s",
                             WTERMSIG(status), argv[0]);
            }
            return;
        }

        if (result < 0) {
            logger_error(monitor->logger, "waitpid() failed: %s", strerror(errno));
            return;
        }

        if (time(NULL) - start_time > 5) {
            logger_warn(monitor->logger, "Shutdown command timeout, killing process");
            kill(child_pid, SIGKILL);
            (void)waitpid(child_pid, &status, 0);
            return;
        }

        usleep(100000);
    }
}

/* 可中断的休眠（支持信号中断和 watchdog 心跳） */
static void sleep_with_stop(monitor_t* monitor, int seconds)
{
    for (int i = 0; i < seconds; i++) {
        if (monitor->stop_flag) {
            break;
        }

        if (monitor_systemd_enabled(monitor) && monitor->config->enable_watchdog) {
            (void)systemd_notifier_watchdog(&monitor->systemd);
        }

        struct timespec ts = {.tv_sec = 1, .tv_nsec = 0};
        while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
            if (monitor->stop_flag) {
                break;
            }
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
                    trigger_shutdown(monitor);
                    monitor->consecutive_fails = 0;
                } else {
                    trigger_shutdown(monitor);
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
