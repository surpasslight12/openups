#include "integrations.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ============================================================
 * systemd 通知器
 * ============================================================ */

static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static bool build_notify_addr(const char* restrict socket_path, struct sockaddr_un* restrict addr,
                              socklen_t* restrict addr_len)
{
    if (socket_path == NULL || addr == NULL || addr_len == NULL) {
        return false;
    }

    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;

    /* 处理抽象命名空间（@ 前缀） */
    if (socket_path[0] == '@') {
        size_t name_len = strlen(socket_path + 1);
        if (name_len == 0 || name_len > sizeof(addr->sun_path) - 1) {
            return false;
        }

        addr->sun_path[0] = '\0';
        memcpy(addr->sun_path + 1, socket_path + 1, name_len);
        *addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + name_len);
        return true;
    }

    size_t path_len = strlen(socket_path);
    if (path_len == 0 || path_len >= sizeof(addr->sun_path)) {
        return false;
    }

    memcpy(addr->sun_path, socket_path, path_len + 1);
    *addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_len + 1);
    return true;
}

/* 发送 systemd 通知消息（READY/STATUS/STOPPING/WATCHDOG）。 */
static bool send_notify(systemd_notifier_t* restrict notifier, const char* restrict message)
{
    if (notifier == NULL || message == NULL || !notifier->enabled) {
        return false;
    }

    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif

    /* 重试发送直到成功或遇到非 EINTR 错误 */
    ssize_t sent;
    do {
        sent = send(notifier->sockfd, message, strlen(message), flags);
    } while (sent < 0 && errno == EINTR);

    return sent >= 0;
}

void systemd_notifier_init(systemd_notifier_t* restrict notifier)
{
    if (notifier == NULL) {
        return;
    }

    notifier->enabled = false;
    notifier->sockfd = -1;
    notifier->watchdog_usec = 0;
    memset(&notifier->addr, 0, sizeof(notifier->addr));
    notifier->addr_len = 0;
    notifier->last_watchdog_ms = 0;
    notifier->last_status_ms = 0;
    notifier->last_status[0] = '\0';

    /* 非 systemd 管理时通常不会设置 NOTIFY_SOCKET。 */
    const char* socket_path = getenv("NOTIFY_SOCKET");
    if (socket_path == NULL) {
        return;
    }

    /* systemd 通知使用 AF_UNIX/SOCK_DGRAM。 */
    int socket_type = SOCK_DGRAM;
#ifdef SOCK_CLOEXEC
    socket_type |= SOCK_CLOEXEC;
#endif
    notifier->sockfd = socket(AF_UNIX, socket_type, 0);
    if (notifier->sockfd < 0) {
        return;
    }

    if (!build_notify_addr(socket_path, &notifier->addr, &notifier->addr_len)) {
        close(notifier->sockfd);
        notifier->sockfd = -1;
        return;
    }

    int rc;
    do {
        rc = connect(notifier->sockfd, (const struct sockaddr*)&notifier->addr, notifier->addr_len);
    } while (rc < 0 && errno == EINTR);
    if (rc < 0) {
        close(notifier->sockfd);
        notifier->sockfd = -1;
        return;
    }

    notifier->enabled = true;

    /* watchdog 超时时间（微秒）；用于计算心跳间隔。 */
    const char* watchdog_str = getenv("WATCHDOG_USEC");
    if (watchdog_str != NULL) {
        notifier->watchdog_usec = strtoull(watchdog_str, NULL, 10);
    }
}

void systemd_notifier_destroy(systemd_notifier_t* restrict notifier)
{
    if (notifier == NULL) {
        return;
    }

    if (notifier->sockfd >= 0) {
        close(notifier->sockfd);
        notifier->sockfd = -1;
    }

    notifier->enabled = false;
    memset(&notifier->addr, 0, sizeof(notifier->addr));
    notifier->addr_len = 0;
}

/* 检查 systemd 通知器是否已启用 */
bool systemd_notifier_is_enabled(const systemd_notifier_t* restrict notifier)
{
    return notifier != NULL && notifier->enabled;
}

/* 通知 systemd 程序已就绪 (启动完成) */
bool systemd_notifier_ready(systemd_notifier_t* restrict notifier)
{
    return send_notify(notifier, "READY=1");
}

/* 更新程序状态信息到 systemd */
bool systemd_notifier_status(systemd_notifier_t* restrict notifier, const char* restrict status)
{
    if (notifier == NULL || !notifier->enabled || status == NULL) {
        return false;
    }

    /* 降频：内容未变化且距离上次发送太近时跳过 */
    uint64_t now_ms = monotonic_ms();
    bool same = (strncmp(notifier->last_status, status, sizeof(notifier->last_status)) == 0);
    if (same && notifier->last_status_ms != 0 && now_ms - notifier->last_status_ms < 2000) {
        return true;
    }

    /* systemd 约定：STATUS=... */
    char message[256];
    snprintf(message, sizeof(message), "STATUS=%s", status);

    bool ok = send_notify(notifier, message);
    if (ok) {
        snprintf(notifier->last_status, sizeof(notifier->last_status), "%s", status);
        notifier->last_status_ms = now_ms;
    }
    return ok;
}

/* 通知 systemd 程序即将停止运行 */
bool systemd_notifier_stopping(systemd_notifier_t* restrict notifier)
{
    return send_notify(notifier, "STOPPING=1");
}

bool systemd_notifier_watchdog(systemd_notifier_t* restrict notifier)
{
    if (notifier == NULL || !notifier->enabled) {
        return false;
    }

    /* 未设置 WatchdogSec 时 systemd 不会提供 WATCHDOG_USEC，此时应默认 no-op。 */
    if (notifier->watchdog_usec == 0) {
        return true;
    }

    uint64_t now_ms = monotonic_ms();
    uint64_t interval_ms = notifier->watchdog_usec / 2000ULL; /* usec/2 -> ms */
    if (interval_ms == 0) {
        interval_ms = 1;
    }

    if (notifier->last_watchdog_ms != 0 && now_ms - notifier->last_watchdog_ms < interval_ms) {
        return true;
    }

    bool ok = send_notify(notifier, "WATCHDOG=1");
    if (ok) {
        notifier->last_watchdog_ms = now_ms;
    }
    return ok;
}

uint64_t systemd_notifier_watchdog_interval_ms(const systemd_notifier_t* restrict notifier)
{
    if (notifier == NULL || !notifier->enabled || notifier->watchdog_usec == 0) {
        return 0;
    }

    uint64_t interval_ms = notifier->watchdog_usec / 2000ULL; /* usec/2 -> ms */
    if (interval_ms == 0) {
        interval_ms = 1;
    }
    return interval_ms;
}

/* ============================================================
 * shutdown 关机触发
 * ============================================================ */

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

    /* 严格拒绝引号/反引号与控制字符。 */
    if (strchr(command, '"') != NULL || strchr(command, '\'') != NULL ||
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

static bool shutdown_select_command(const config_t* restrict config, bool use_systemctl_poweroff,
                                    char* restrict command_buf, size_t command_size)
{
    if (config == NULL || command_buf == NULL || command_size == 0) {
        return false;
    }

    if (config->shutdown_mode == SHUTDOWN_MODE_IMMEDIATE) {
        if (use_systemctl_poweroff) {
            snprintf(command_buf, command_size, "systemctl poweroff");
        } else {
            snprintf(command_buf, command_size, "/sbin/shutdown -h now");
        }
        return true;
    }

    if (config->shutdown_mode == SHUTDOWN_MODE_DELAYED) {
        snprintf(command_buf, command_size, "/sbin/shutdown -h +%d", config->delay_minutes);
        return true;
    }

    return false;
}

static void shutdown_log_planned(logger_t* restrict logger, shutdown_mode_t mode,
                                 int delay_minutes)
{
    if (logger == NULL) {
        return;
    }

    if (mode == SHUTDOWN_MODE_IMMEDIATE) {
        logger_warn(logger, "Triggering immediate shutdown");
    } else if (mode == SHUTDOWN_MODE_DELAYED) {
        logger_warn(logger, "Triggering shutdown in %d minutes", delay_minutes);
    }
}

static bool shutdown_should_execute(const config_t* restrict config, logger_t* restrict logger)
{
    if (config == NULL || logger == NULL) {
        return false;
    }

    if (config->dry_run) {
        logger_info(logger, "[DRY-RUN] Would trigger shutdown in %s mode",
                    shutdown_mode_to_string(config->shutdown_mode));
        return false;
    }

    if (config->shutdown_mode == SHUTDOWN_MODE_LOG_ONLY) {
        logger_error(logger, "LOG-ONLY mode: Network connectivity lost, would trigger shutdown");
        return false;
    }

    return true;
}

static bool shutdown_execute_command(char* argv[], logger_t* restrict logger)
{
    if (argv == NULL || argv[0] == NULL || logger == NULL) {
        return false;
    }

    /* 使用 fork/execvp 执行（不经过 shell）。 */
    pid_t child_pid = fork();
    if (child_pid < 0) {
        logger_error(logger, "fork() failed: %s", strerror(errno));
        return false;
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

    /* 父进程：等待子进程完成（5 秒超时）。 */
    int status = 0;
    uint64_t start_ms = get_monotonic_ms();
    bool use_monotonic = start_ms != UINT64_MAX;
    if (!use_monotonic) {
        start_ms = (uint64_t)time(NULL) * UINT64_C(1000);
    }
    while (1) {
        pid_t result = waitpid(child_pid, &status, WNOHANG);
        if (result == child_pid) {
            if (WIFEXITED(status)) {
                int exit_code = WEXITSTATUS(status);
                if (exit_code == 0) {
                    logger_info(logger, "Shutdown command executed successfully");
                } else {
                    logger_error(logger, "Shutdown command failed with exit code %d: %s",
                                 exit_code, argv[0]);
                }
            } else if (WIFSIGNALED(status)) {
                logger_error(logger, "Shutdown command terminated by signal %d: %s",
                             WTERMSIG(status), argv[0]);
            }
            return true;
        }

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            logger_error(logger, "waitpid() failed: %s", strerror(errno));
            return false;
        }

        uint64_t now_ms = use_monotonic ? get_monotonic_ms()
                                        : (uint64_t)time(NULL) * UINT64_C(1000);
        if (now_ms == UINT64_MAX) {
            now_ms = (uint64_t)time(NULL) * UINT64_C(1000);
            use_monotonic = false;
        }

        if (now_ms - start_ms > UINT64_C(5000)) {
            logger_warn(logger, "Shutdown command timeout, killing process");
            kill(child_pid, SIGKILL);
            (void)waitpid(child_pid, &status, 0);
            return false;
        }

        usleep(100000);
    }
}

void shutdown_trigger(const config_t* config, logger_t* logger, bool use_systemctl_poweroff)
{
    if (config == NULL || logger == NULL) {
        return;
    }

    logger_warn(logger, "Shutdown threshold reached, mode is %s%s",
                shutdown_mode_to_string(config->shutdown_mode),
                config->dry_run ? " (dry-run enabled)" : "");

    if (!shutdown_should_execute(config, logger)) {
        return;
    }

    char command_buf[512];
    char* argv[16] = {0};

    if (!shutdown_select_command(config, use_systemctl_poweroff, command_buf,
                                 sizeof(command_buf))) {
        logger_error(logger, "Unknown shutdown mode");
        return;
    }

    if (!build_command_argv(command_buf, command_buf, sizeof(command_buf), argv,
                            sizeof(argv) / sizeof(argv[0]))) {
        logger_error(logger, "Failed to parse shutdown command: %s", command_buf);
        return;
    }

    shutdown_log_planned(logger, config->shutdown_mode, config->delay_minutes);

    (void)shutdown_execute_command(argv, logger);
}
