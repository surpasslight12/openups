#include "shutdown.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ============================================================
 * shutdown 关机触发
 * ============================================================ */

/* 构建关机命令参数数组（空白分隔，不支持引号） */
static bool build_shutdown_argv(const char* command, char* buffer, size_t buffer_size,
                                char* argv[], size_t argv_size)
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

static void log_shutdown_plan(logger_t* restrict logger, shutdown_mode_t mode,
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

static OPENUPS_COLD bool shutdown_should_execute(const config_t* restrict config, logger_t* restrict logger)
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

static OPENUPS_COLD bool shutdown_execute_command(char* argv[], logger_t* restrict logger)
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

    if (!build_shutdown_argv(command_buf, command_buf, sizeof(command_buf), argv,
                             sizeof(argv) / sizeof(argv[0]))) {
        logger_error(logger, "Failed to parse shutdown command: %s", command_buf);
        return;
    }

    log_shutdown_plan(logger, config->shutdown_mode, config->delay_minutes);

    (void)shutdown_execute_command(argv, logger);
}
