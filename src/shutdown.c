#include "shutdown.h"
#include "common.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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

void shutdown_trigger(const config_t* config, logger_t* logger, bool use_systemctl)
{
    if (config == NULL || logger == NULL) {
        return;
    }

    logger_warn(logger, "Shutdown threshold reached, mode is %s%s",
                shutdown_mode_to_string(config->shutdown_mode),
                config->dry_run ? " (dry-run enabled)" : "");

    if (config->dry_run) {
        logger_info(logger, "[DRY-RUN] Would trigger shutdown in %s mode",
                    shutdown_mode_to_string(config->shutdown_mode));
        return;
    }

    if (config->shutdown_mode == SHUTDOWN_MODE_LOG_ONLY) {
        logger_error(logger, "LOG-ONLY mode: Network connectivity lost, would trigger shutdown");
        return;
    }

    char command_buf[512];
    char* argv[16] = {0};

    /* Default to userspace shutdown paths (no direct reboot syscall) */
    if (config->shutdown_mode == SHUTDOWN_MODE_IMMEDIATE) {
        if (use_systemctl) {
            snprintf(command_buf, sizeof(command_buf), "systemctl poweroff");
        } else {
            snprintf(command_buf, sizeof(command_buf), "/sbin/shutdown -h now");
        }
    } else if (config->shutdown_mode == SHUTDOWN_MODE_DELAYED) {
        snprintf(command_buf, sizeof(command_buf), "/sbin/shutdown -h +%d", config->delay_minutes);
    } else {
        logger_error(logger, "Unknown shutdown mode");
        return;
    }

    if (!build_command_argv(command_buf, command_buf, sizeof(command_buf), argv,
                            sizeof(argv) / sizeof(argv[0]))) {
        logger_error(logger, "Failed to parse shutdown command: %s", command_buf);
        return;
    }

    if (config->shutdown_mode == SHUTDOWN_MODE_IMMEDIATE) {
        logger_warn(logger, "Triggering immediate shutdown");
    } else {
        logger_warn(logger, "Triggering shutdown in %d minutes", config->delay_minutes);
    }

    /* 执行关机命令（使用 fork/execvp 以避免 shell 注入） */
    pid_t child_pid = fork();
    if (child_pid < 0) {
        logger_error(logger, "fork() failed: %s", strerror(errno));
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
                    logger_info(logger, "Shutdown command executed successfully");
                } else {
                    logger_error(logger, "Shutdown command failed with exit code %d: %s", exit_code,
                                 argv[0]);
                }
            } else if (WIFSIGNALED(status)) {
                logger_error(logger, "Shutdown command terminated by signal %d: %s", WTERMSIG(status),
                             argv[0]);
            }
            return;
        }

        if (result < 0) {
            logger_error(logger, "waitpid() failed: %s", strerror(errno));
            return;
        }

        if (time(NULL) - start_time > 5) {
            logger_warn(logger, "Shutdown command timeout, killing process");
            kill(child_pid, SIGKILL);
            (void)waitpid(child_pid, &status, 0);
            return;
        }

        usleep(100000);
    }
}
