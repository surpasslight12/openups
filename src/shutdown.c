#include "openups.h"
#include <spawn.h>

#define OPENUPS_SHUTDOWN_STARTUP_GRACE_MS 1000U
#define OPENUPS_SHUTDOWN_POLL_INTERVAL_NS 50000000L

typedef enum {
  SHUTDOWN_BACKEND_SYSTEMCTL = 0,
  SHUTDOWN_BACKEND_SHUTDOWN = 1,
} shutdown_backend_t;

static shutdown_backend_t select_shutdown_backend(bool use_systemctl_poweroff) {
  return use_systemctl_poweroff ? SHUTDOWN_BACKEND_SYSTEMCTL
                                : SHUTDOWN_BACKEND_SHUTDOWN;
}

static const char *shutdown_backend_command(shutdown_backend_t backend) {
  return backend == SHUTDOWN_BACKEND_SYSTEMCTL ? "/usr/bin/systemctl"
                                               : "/sbin/shutdown";
}

static bool shutdown_build_delay_arg(shutdown_backend_t backend,
                                     int delay_minutes,
                                     char *restrict delay_arg,
                                     size_t delay_arg_size) {
  if (delay_arg == NULL || delay_arg_size == 0 || delay_minutes <= 0) {
    return false;
  }

  int written = 0;
  if (backend == SHUTDOWN_BACKEND_SYSTEMCTL) {
    written = snprintf(delay_arg, delay_arg_size, "+%dmin", delay_minutes);
  } else {
    written = snprintf(delay_arg, delay_arg_size, "+%d", delay_minutes);
  }

  return written > 0 && (size_t)written < delay_arg_size;
}

static bool shutdown_select_argv(const config_t *restrict config,
                                 bool use_systemctl_poweroff,
                                 char *restrict delay_arg,
                                 size_t delay_arg_size,
                                 char *argv[], size_t argv_size) {
  if (config == NULL || delay_arg == NULL || delay_arg_size == 0 ||
      argv == NULL || argv_size < 5) {
    return false;
  }

  memset(argv, 0, sizeof(argv[0]) * argv_size);
  delay_arg[0] = '\0';

  shutdown_backend_t backend =
      select_shutdown_backend(use_systemctl_poweroff);
  argv[0] = (char *)shutdown_backend_command(backend);

  if (config->shutdown_mode == SHUTDOWN_MODE_IMMEDIATE) {
    if (backend == SHUTDOWN_BACKEND_SYSTEMCTL) {
      argv[1] = "poweroff";
    } else {
      argv[1] = "-h";
      argv[2] = "now";
    }
    return true;
  }

  if (config->shutdown_mode == SHUTDOWN_MODE_DELAYED) {
    if (!shutdown_build_delay_arg(backend, config->delay_minutes, delay_arg,
                                  delay_arg_size)) {
      return false;
    }

    if (backend == SHUTDOWN_BACKEND_SYSTEMCTL) {
      argv[1] = "--when";
      argv[2] = delay_arg;
      argv[3] = "poweroff";
    } else {
      argv[1] = "-h";
      argv[2] = delay_arg;
    }
    return true;
  }

  return false;
}

static bool shutdown_should_execute(const config_t *restrict config,
                                    logger_t *restrict logger) {
  if (config == NULL || logger == NULL) {
    return false;
  }

  if (config->dry_run) {
    logger_info(logger, "[DRY-RUN] Would trigger shutdown in %s mode",
                shutdown_mode_to_string(config->shutdown_mode));
    return false;
  }

  return true;
}

static bool shutdown_sleep_retry_window(void) {
  struct timespec remaining = {.tv_sec = 0,
                               .tv_nsec = OPENUPS_SHUTDOWN_POLL_INTERVAL_NS};
  while (nanosleep(&remaining, &remaining) < 0) {
    if (errno != EINTR) {
      return false;
    }
  }
  return true;
}

static shutdown_result_t shutdown_observe_startup(
    pid_t child_pid, const char *restrict command_path,
    const logger_t *restrict logger) {
  if (child_pid <= 0 || command_path == NULL || logger == NULL) {
    return SHUTDOWN_RESULT_FAILED;
  }

  uint64_t start_ms = get_monotonic_ms();
  if (start_ms == UINT64_MAX) {
    logger_info(logger, "Shutdown command started successfully");
    return SHUTDOWN_RESULT_TRIGGERED;
  }

  uint64_t deadline_ms = start_ms + OPENUPS_SHUTDOWN_STARTUP_GRACE_MS;
  int status = 0;
  while (true) {
    pid_t result = waitpid(child_pid, &status, WNOHANG);
    if (result == child_pid) {
      if (WIFEXITED(status)) {
        int exit_code = WEXITSTATUS(status);
        if (exit_code == 0) {
          logger_info(logger, "Shutdown command executed successfully");
          return SHUTDOWN_RESULT_TRIGGERED;
        }

        logger_error(logger, "Shutdown command failed with exit code %d: %s",
                     exit_code, command_path);
        return SHUTDOWN_RESULT_FAILED;
      }

      if (WIFSIGNALED(status)) {
        logger_error(logger, "Shutdown command terminated by signal %d",
                     WTERMSIG(status));
        return SHUTDOWN_RESULT_FAILED;
      }

      logger_error(logger, "Shutdown command exited unexpectedly");
      return SHUTDOWN_RESULT_FAILED;
    }

    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }

      logger_error(logger, "waitpid() failed: %s", strerror(errno));
      return SHUTDOWN_RESULT_FAILED;
    }

    uint64_t now_ms = get_monotonic_ms();
    if (now_ms == UINT64_MAX || now_ms >= deadline_ms) {
      logger_info(logger, "Shutdown command started successfully");
      return SHUTDOWN_RESULT_TRIGGERED;
    }

    if (!shutdown_sleep_retry_window()) {
      logger_info(logger, "Shutdown command started successfully");
      return SHUTDOWN_RESULT_TRIGGERED;
    }
  }
}

static shutdown_result_t shutdown_execute_command(
    char *argv[], const logger_t *restrict logger) {
  if (argv == NULL || argv[0] == NULL || logger == NULL) {
    return SHUTDOWN_RESULT_FAILED;
  }

  static char *const shutdown_envp[] = {"PATH=/usr/bin:/usr/sbin:/bin:/sbin",
                                        "LANG=C", "LC_ALL=C", NULL};

  posix_spawn_file_actions_t file_actions;
  int rc = posix_spawn_file_actions_init(&file_actions);
  if (rc != 0) {
    logger_error(logger, "posix_spawn_file_actions_init failed: %s",
                 strerror(rc));
    return SHUTDOWN_RESULT_FAILED;
  }

  rc = posix_spawn_file_actions_addopen(&file_actions, STDIN_FILENO,
                                        "/dev/null", O_RDONLY, 0);
  if (rc == 0) {
    rc = posix_spawn_file_actions_addopen(&file_actions, STDOUT_FILENO,
                                          "/dev/null", O_WRONLY, 0);
  }
  if (rc == 0) {
    rc = posix_spawn_file_actions_addopen(&file_actions, STDERR_FILENO,
                                          "/dev/null", O_WRONLY, 0);
  }
  if (rc != 0) {
    logger_error(logger, "Failed to prepare stdio redirection: %s",
                 strerror(rc));
    posix_spawn_file_actions_destroy(&file_actions);
    return SHUTDOWN_RESULT_FAILED;
  }

  pid_t child_pid = -1;
  int spawn_err =
      posix_spawn(&child_pid, argv[0], &file_actions, NULL, argv, shutdown_envp);

  posix_spawn_file_actions_destroy(&file_actions);

  if (spawn_err != 0) {
    logger_error(logger, "posix_spawn failed: %s", strerror(spawn_err));
    return SHUTDOWN_RESULT_FAILED;
  }

  return shutdown_observe_startup(child_pid, argv[0], logger);
}

shutdown_result_t shutdown_trigger(const config_t *config, logger_t *logger,
                                   bool use_systemctl_poweroff) {
  if (config == NULL || logger == NULL) {
    return SHUTDOWN_RESULT_FAILED;
  }

  if (config->shutdown_mode == SHUTDOWN_MODE_LOG_ONLY) {
    logger_warn(logger,
                "Failure threshold reached in LOG-ONLY mode; continuing monitoring without shutdown");
    return SHUTDOWN_RESULT_NO_ACTION;
  }

  logger_warn(logger, "Shutdown threshold reached, mode is %s%s",
              shutdown_mode_to_string(config->shutdown_mode),
              config->dry_run ? " (dry-run enabled)" : "");

  if (!shutdown_should_execute(config, logger)) {
    return SHUTDOWN_RESULT_NO_ACTION;
  }

  char delay_arg[32];
  char *argv[5] = {0};
  if (!shutdown_select_argv(config, use_systemctl_poweroff, delay_arg,
                            sizeof(delay_arg), argv,
                            sizeof(argv) / sizeof(argv[0]))) {
    logger_error(logger, "Failed to build shutdown command arguments");
    return SHUTDOWN_RESULT_FAILED;
  }

  log_shutdown_plan(logger, config->shutdown_mode, config->delay_minutes);
  return shutdown_execute_command(argv, logger);
}