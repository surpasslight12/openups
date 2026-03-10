#include "openups.h"

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>

#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

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

static bool shutdown_select_argv(const config_t *restrict config,
                                 bool use_systemctl_poweroff,
                                 char *argv[], size_t argv_size) {
  if (config == NULL || argv == NULL || argv_size < 4) {
    return false;
  }

  memset(argv, 0, sizeof(argv[0]) * argv_size);

  shutdown_backend_t backend = select_shutdown_backend(use_systemctl_poweroff);
  argv[0] = (char *)shutdown_backend_command(backend);

  if (config->shutdown_mode == SHUTDOWN_MODE_TRUE_OFF) {
    if (backend == SHUTDOWN_BACKEND_SYSTEMCTL) {
      argv[1] = "poweroff";
    } else {
      argv[1] = "-h";
      argv[2] = "now";
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

  if (config->shutdown_mode == SHUTDOWN_MODE_DRY_RUN) {
    logger_info(logger, "[DRY-RUN] Would trigger shutdown now");
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

static shutdown_result_t shutdown_consume_child_status(
    pid_t child_pid, pid_t wait_result, int status,
    const char *restrict command_path, const logger_t *restrict logger) {
  if (wait_result != child_pid) {
    return SHUTDOWN_RESULT_NO_ACTION;
  }

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

static shutdown_result_t shutdown_observe_startup(
    pid_t child_pid, const char *restrict command_path,
    const logger_t *restrict logger) {
  if (child_pid <= 0 || command_path == NULL || logger == NULL) {
    return SHUTDOWN_RESULT_FAILED;
  }

  uint64_t start_ms = get_monotonic_ms();
  if (start_ms == UINT64_MAX) {
    int status = 0;
    pid_t result = waitpid(child_pid, &status, WNOHANG);
    if (result < 0) {
      if (errno == EINTR) {
        logger_warn(logger,
                    "Monotonic clock unavailable while observing shutdown startup; assuming command started");
        return SHUTDOWN_RESULT_TRIGGERED;
      }

      logger_error(logger, "waitpid() failed: %s", strerror(errno));
      return SHUTDOWN_RESULT_FAILED;
    }

    shutdown_result_t immediate_result = shutdown_consume_child_status(
        child_pid, result, status, command_path, logger);
    if (immediate_result != SHUTDOWN_RESULT_NO_ACTION) {
      return immediate_result;
    }

    logger_warn(logger,
                "Monotonic clock unavailable while observing shutdown startup; assuming command started");
    return SHUTDOWN_RESULT_TRIGGERED;
  }

  uint64_t deadline_ms = start_ms + OPENUPS_SHUTDOWN_STARTUP_GRACE_MS;
  int status = 0;
  while (true) {
    pid_t result = waitpid(child_pid, &status, WNOHANG);
    shutdown_result_t child_result = shutdown_consume_child_status(
        child_pid, result, status, command_path, logger);
    if (child_result != SHUTDOWN_RESULT_NO_ACTION) {
      return child_result;
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

  logger_warn(logger, "Shutdown threshold reached, mode is %s",
              shutdown_mode_to_string(config->shutdown_mode));

  if (!shutdown_should_execute(config, logger)) {
    return SHUTDOWN_RESULT_NO_ACTION;
  }

  char *argv[4] = {0};
  if (!shutdown_select_argv(config, use_systemctl_poweroff, argv,
                            sizeof(argv) / sizeof(argv[0]))) {
    logger_error(logger, "Failed to build shutdown command arguments");
    return SHUTDOWN_RESULT_FAILED;
  }

  logger_warn(logger, "Triggering shutdown now");
  return shutdown_execute_command(argv, logger);
}