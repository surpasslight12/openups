#include "openups.h"

static bool build_shutdown_argv(const char *command, char *buffer,
                                size_t buffer_size, char *argv[],
                                size_t argv_size) {
  if (command == NULL || buffer == NULL || argv == NULL || argv_size < 2) {
    return false;
  }

  size_t cmd_len = strlen(command);
  if (cmd_len == 0 || cmd_len >= buffer_size) {
    return false;
  }

  /* Reject quotes, backticks and control characters */
  if (strchr(command, '"') != NULL || strchr(command, '\'') != NULL ||
      strchr(command, '`') != NULL) {
    return false;
  }

  for (const char *p = command; *p != '\0'; ++p) {
    if ((unsigned char)*p < 0x20 || *p == 0x7F) {
      return false;
    }
  }

  memcpy(buffer, command, cmd_len + 1);

  size_t argc = 0;
  char *saveptr = NULL;
  char *token = strtok_r(buffer, " \t", &saveptr);
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

static bool shutdown_select_command(const config_t *restrict config,
                                    bool use_systemctl_poweroff,
                                    char *restrict command_buf,
                                    size_t command_size) {
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
    snprintf(command_buf, command_size, "/sbin/shutdown -h +%d",
             config->delay_minutes);
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

  if (config->shutdown_mode == SHUTDOWN_MODE_LOG_ONLY) {
    logger_error(
        logger,
        "LOG-ONLY mode: Network connectivity lost, would trigger shutdown");
    return false;
  }

  return true;
}

static bool shutdown_execute_command(char *argv[],
                                     const logger_t *restrict logger) {
  if (argv == NULL || argv[0] == NULL || logger == NULL) {
    return false;
  }

  posix_spawn_file_actions_t file_actions;
  if (posix_spawn_file_actions_init(&file_actions) != 0) {
    logger_error(logger, "posix_spawn_file_actions_init failed: %s", strerror(errno));
    return false;
  }

  /* Redirect stdin/stdout/stderr to /dev/null */
  (void)posix_spawn_file_actions_addopen(&file_actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  (void)posix_spawn_file_actions_addopen(&file_actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
  (void)posix_spawn_file_actions_addopen(&file_actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

  extern char **environ;
  pid_t child_pid = -1;
  int spawn_err = posix_spawnp(&child_pid, argv[0], &file_actions, NULL, argv, environ);

  posix_spawn_file_actions_destroy(&file_actions);

  if (spawn_err != 0) {
    logger_error(logger, "posix_spawnp() failed: %s", strerror(spawn_err));
    return false;
  }

  /* Parent process: poll-wait up to 50 × 100ms = 5 seconds */
  int status = 0;
  for (int poll_count = 0; poll_count < 50; poll_count++) {
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

    struct timespec poll_ts = {.tv_sec = 0, .tv_nsec = 100000000L};
    (void)nanosleep(&poll_ts, NULL);
  }

  logger_warn(logger, "Shutdown command timeout, killing process");
  kill(child_pid, SIGKILL);
  (void)waitpid(child_pid, &status, 0);
  return false;
}

/* Orchestrate the full shutdown sequence: guard checks → command selection →
 * execution. */
static void shutdown_trigger(const config_t *config, logger_t *logger,
                             bool use_systemctl_poweroff) {
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
  char *argv[16] = {0};

  if (!shutdown_select_command(config, use_systemctl_poweroff, command_buf,
                               sizeof(command_buf))) {
    logger_error(logger, "Unknown shutdown mode");
    return;
  }

  /* Separate buffer for build_shutdown_argv in-place tokenisation (cannot reuse
   * command_buf: violates restrict) */
  char argv_buf[512];
  if (!build_shutdown_argv(command_buf, argv_buf, sizeof(argv_buf), argv,
                           sizeof(argv) / sizeof(argv[0]))) {
    logger_error(logger, "Failed to parse shutdown command: %s", command_buf);
    return;
  }

  log_shutdown_plan(logger, config->shutdown_mode, config->delay_minutes);

  (void)shutdown_execute_command(argv, logger);
}

/* Populate a sockaddr_un from NOTIFY_SOCKET path, handling abstract namespace
 * (@-prefix). */
static bool build_systemd_addr(const char *restrict socket_path,
                               struct sockaddr_un *restrict addr,
                               socklen_t *restrict addr_len) {
  if (socket_path == NULL || addr == NULL || addr_len == NULL) {
    return false;
  }

  memset(addr, 0, sizeof(*addr));
  addr->sun_family = AF_UNIX;

  /* Abstract namespace socket (@ prefix) */
  if (socket_path[0] == '@') {
    size_t name_len = strlen(socket_path + 1);
    if (name_len == 0 || name_len > sizeof(addr->sun_path) - 1) {
      return false;
    }

    addr->sun_path[0] = '\0';
    memcpy(addr->sun_path + 1, socket_path + 1, name_len);
    *addr_len =
        (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + name_len);
    return true;
  }

  size_t path_len = strlen(socket_path);
  if (path_len == 0 || path_len >= sizeof(addr->sun_path)) {
    return false;
  }

  memcpy(addr->sun_path, socket_path, path_len + 1);
  *addr_len =
      (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_len + 1);
  return true;
}

/* Send a sd_notify datagram; skips silently if notifier is disabled. */
static bool send_notify(systemd_notifier_t *restrict notifier,
                        const char *restrict message) {
  if (OPENUPS_UNLIKELY(notifier == NULL || message == NULL ||
                       !notifier->enabled)) {
    return false;
  }

  ssize_t sent;
  do {
    sent = send(notifier->sockfd, message, strlen(message), MSG_NOSIGNAL);
  } while (sent < 0 && errno == EINTR);
  return sent >= 0;
}

/* Init notifier from NOTIFY_SOCKET / WATCHDOG_USEC env vars; no-op if not under
 * systemd. */
static void systemd_notifier_init(systemd_notifier_t *restrict notifier) {
  if (notifier == NULL) {
    return;
  }

  notifier->enabled = false;
  notifier->sockfd = -1;
  notifier->watchdog_usec = 0;
  notifier->last_watchdog_ms = 0;
  notifier->last_status_ms = 0;
  notifier->last_status[0] = '\0';

  /* NOTIFY_SOCKET is unset when not running under systemd */
  const char *socket_path = getenv("NOTIFY_SOCKET");
  if (socket_path == NULL) {
    return;
  }

  /* systemd notifications use AF_UNIX/SOCK_DGRAM */
  notifier->sockfd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (notifier->sockfd < 0) {
    return;
  }

  struct sockaddr_un addr;
  socklen_t addr_len;
  if (!build_systemd_addr(socket_path, &addr, &addr_len)) {
    close(notifier->sockfd);
    notifier->sockfd = -1;
    return;
  }

  int rc;
  do {
    rc = connect(notifier->sockfd, (const struct sockaddr *)&addr, addr_len);
  } while (rc < 0 && errno == EINTR);
  if (rc < 0) {
    close(notifier->sockfd);
    notifier->sockfd = -1;
    return;
  }

  notifier->enabled = true;

  /* WATCHDOG_USEC is set by systemd in microseconds; keep 0 (disabled) if
   * absent or invalid */
  const char *watchdog_str = getenv("WATCHDOG_USEC");
  if (watchdog_str != NULL) {
    errno = 0;
    char *endptr = NULL;
    unsigned long long val = strtoull(watchdog_str, &endptr, 10);
    if (errno == 0 && endptr != watchdog_str && *endptr == '\0') {
      notifier->watchdog_usec = (uint64_t)val;
    }
  }
}

/* Close the UNIX socket used for sd_notify. */
static void systemd_notifier_destroy(systemd_notifier_t *restrict notifier) {
  if (notifier == NULL) {
    return;
  }

  if (notifier->sockfd >= 0) {
    close(notifier->sockfd);
    notifier->sockfd = -1;
  }

  notifier->enabled = false;
}

/* Return true if the notifier successfully connected to NOTIFY_SOCKET. */
static bool
systemd_notifier_is_enabled(const systemd_notifier_t *restrict notifier) {
  return notifier != NULL && notifier->enabled;
}

/* Send READY=1 to signal successful startup. */
static bool systemd_notifier_ready(systemd_notifier_t *restrict notifier) {
  return send_notify(notifier, "READY=1");
}

/* Send STATUS=<status> to systemd; rate-limited to avoid redundant updates. */
static bool systemd_notifier_status(systemd_notifier_t *restrict notifier,
                                    const char *restrict status) {
  if (notifier == NULL || !notifier->enabled || status == NULL) {
    return false;
  }

  /* Rate-limit: skip if content is unchanged and fewer than 2 seconds have
   * elapsed */
  uint64_t now_ms = get_monotonic_ms();
  bool same = (strncmp(notifier->last_status, status,
                       sizeof(notifier->last_status)) == 0);
  if (same && notifier->last_status_ms != 0 &&
      now_ms - notifier->last_status_ms < 2000) {
    return true;
  }

  char message[256];
  snprintf(message, sizeof(message), "STATUS=%s", status);
  bool ok = send_notify(notifier, message);
  if (ok) {
    snprintf(notifier->last_status, sizeof(notifier->last_status), "%s",
             status);
    notifier->last_status_ms = now_ms;
  }
  return ok;
}

/* Send STOPPING=1 immediately before the process exits. */
static bool systemd_notifier_stopping(systemd_notifier_t *restrict notifier) {
  return send_notify(notifier, "STOPPING=1");
}

/* Send WATCHDOG=1 when the kick interval has elapsed; no-op if watchdog is
 * unconfigured. */
static bool systemd_notifier_watchdog(systemd_notifier_t *restrict notifier) {
  if (OPENUPS_UNLIKELY(notifier == NULL || !notifier->enabled)) {
    return false;
  }

  /* Skip if WatchdogSec is not configured (watchdog_usec == 0) */
  if (OPENUPS_UNLIKELY(notifier->watchdog_usec == 0)) {
    return true;
  }

  uint64_t now_ms = get_monotonic_ms();
  uint64_t interval_ms = notifier->watchdog_usec / 2000ULL; /* usec/2 → ms */
  if (interval_ms == 0) {
    interval_ms = 1;
  }

  if (notifier->last_watchdog_ms != 0 &&
      now_ms - notifier->last_watchdog_ms < interval_ms) {
    return true;
  }

  bool ok = send_notify(notifier, "WATCHDOG=1");
  if (ok) {
    notifier->last_watchdog_ms = now_ms;
  }
  return ok;
}

/* Return the recommended watchdog kick interval in milliseconds (WATCHDOG_USEC
 * / 2). */
static uint64_t systemd_notifier_watchdog_interval_ms(
    const systemd_notifier_t *restrict notifier) {
  if (notifier == NULL || !notifier->enabled || notifier->watchdog_usec == 0) {
    return 0;
  }

  uint64_t interval_ms = notifier->watchdog_usec / 2000ULL; /* usec/2 → ms */
  if (interval_ms == 0) {
    interval_ms = 1;
  }
  return interval_ms;
}

/* Format and forward a status string to systemd (printf-style). */
[[gnu::format(printf, 2, 3)]]
static void notify_systemd_status(openups_ctx_t *restrict ctx,
                                  const char *restrict fmt, ...) {
  if (ctx == NULL || !ctx->systemd_enabled || fmt == NULL) {
    return;
  }

  char status_msg[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(status_msg, sizeof(status_msg), fmt, args);
  va_end(args);

  (void)systemd_notifier_status(&ctx->systemd, status_msg);
}

/* Initialise context: load config (CLI > env > defaults), validate, open
 * pinger, connect systemd. */
static bool openups_ctx_init(openups_ctx_t *restrict ctx, int argc,
                             char **restrict argv, char *restrict error_msg,
                             size_t error_size) {
  if (ctx == NULL || error_msg == NULL || error_size == 0) {
    return false;
  }

  memset(ctx, 0, sizeof(*ctx));

  /* Load config: CLI > env vars > defaults */
  config_init_default(&ctx->config);
  config_load_from_env(&ctx->config);

  if (!config_load_from_cmdline(&ctx->config, argc, argv)) {
    snprintf(error_msg, error_size, "Failed to parse command line arguments");
    return false;
  }

  if (!config_validate(&ctx->config, error_msg, error_size)) {
    return false;
  }

  /* Cache PID for hot-path ICMP id matching (avoid repeated getpid syscall) */
  ctx->cached_pid = (uint16_t)(getpid() & 0xFFFF);
  if (ctx->cached_pid == 0)
    ctx->cached_pid = 1;

  /* Logger */
  logger_init(&ctx->logger, ctx->config.log_level,
              ctx->config.enable_timestamp);
  if (ctx->config.log_level == LOG_LEVEL_DEBUG) {
    config_print(&ctx->config, &ctx->logger);
  }

  /* Resolve Target and ICMP pinger */
  ctx->dest_addr_len = sizeof(ctx->dest_addr);
  if (!resolve_target(ctx->config.target, &ctx->dest_addr, &ctx->dest_addr_len,
                      error_msg, error_size)) {
    return false;
  }

  int family = ((struct sockaddr *)&ctx->dest_addr)->sa_family;
  if (!icmp_pinger_init(&ctx->pinger, family, error_msg, error_size)) {
    return false;
  }

  /* Metrics */
  metrics_init(&ctx->metrics);

  /* systemd integration */
  if (ctx->config.enable_systemd) {
    systemd_notifier_init(&ctx->systemd);
    ctx->systemd_enabled = systemd_notifier_is_enabled(&ctx->systemd);

    if (ctx->systemd_enabled) {
      logger_debug(&ctx->logger, "systemd integration enabled");
      if (ctx->config.enable_watchdog) {
        ctx->watchdog_interval_ms =
            systemd_notifier_watchdog_interval_ms(&ctx->systemd);
        logger_debug(&ctx->logger, "watchdog interval: %" PRIu64 "ms",
                     ctx->watchdog_interval_ms);
      }
    } else {
      logger_debug(&ctx->logger, "systemd not detected, integration disabled");
    }
  } else {
    systemd_notifier_destroy(&ctx->systemd);
  }

  return true;
}

/* Release all resources held by context and clear the struct. */
static void openups_ctx_destroy(openups_ctx_t *restrict ctx) {
  if (ctx == NULL) {
    return;
  }

  icmp_pinger_destroy(&ctx->pinger);
  systemd_notifier_destroy(&ctx->systemd);

  memset(ctx, 0, sizeof(*ctx));
}

/* Atomically write the current operational state and metrics to a JSON file. */

/* Emit a one-line statistics summary to the logger (triggered by SIGUSR1 or at
 * shutdown). */
static void openups_ctx_print_stats(openups_ctx_t *restrict ctx) {
  if (ctx == NULL) {
    return;
  }

  const metrics_t *metrics = &ctx->metrics;

  if (metrics->successful_pings > 0) {
    logger_info(&ctx->logger,
                "Statistics: %" PRIu64 " total pings, %" PRIu64
                " successful, %" PRIu64
                " failed (%.2f%% success rate), latency min %.2fms / max "
                "%.2fms / avg %.2fms,"
                " uptime %" PRIu64 " seconds",
                metrics->total_pings, metrics->successful_pings,
                metrics->failed_pings, metrics_success_rate(metrics),
                metrics->min_latency, metrics->max_latency,
                metrics_avg_latency(metrics), metrics_uptime_seconds(metrics));
  } else {
    logger_info(&ctx->logger,
                "Statistics: %" PRIu64 " total pings, 0 successful, %" PRIu64
                " failed (0.00%% success rate), latency N/A, uptime %" PRIu64
                " seconds",
                metrics->total_pings, metrics->failed_pings,
                metrics_uptime_seconds(metrics));
  }
}

/* Handle a successful ping: reset fail counter, record metrics, update systemd
 * status. */
static void handle_ping_success(openups_ctx_t *restrict ctx,
                                const ping_result_t *restrict result) {
  if (ctx == NULL || result == NULL) {
    return;
  }

  ctx->consecutive_fails = 0;
  metrics_record_success(&ctx->metrics, result->latency_ms);

  logger_debug(&ctx->logger, "Ping successful to %s, latency: %.2fms",
               ctx->config.target, result->latency_ms);

  /* Update systemd status */
  if (ctx->systemd_enabled) {
    const metrics_t *metrics = &ctx->metrics;
    double success_rate = metrics_success_rate(metrics);
    notify_systemd_status(
        ctx, "OK: %" PRIu64 "/%" PRIu64 " pings (%.1f%%), latency %.2fms",
        metrics->successful_pings, metrics->total_pings, success_rate,
        result->latency_ms);
  }
}

/* Handle a failed ping: increment fail counter, record metrics, update systemd
 * status. */
static void handle_ping_failure(openups_ctx_t *restrict ctx,
                                const ping_result_t *restrict result) {
  if (ctx == NULL || result == NULL) {
    return;
  }

  ctx->consecutive_fails++;
  metrics_record_failure(&ctx->metrics);

  logger_warn(&ctx->logger, "Ping failed to %s: %s (consecutive failures: %d)",
              ctx->config.target, result->error_msg, ctx->consecutive_fails);

  /* Update systemd status */
  if (ctx->systemd_enabled) {
    notify_systemd_status(ctx,
                          "WARNING: %d consecutive failures, threshold is %d",
                          ctx->consecutive_fails, ctx->config.fail_threshold);
  }
}

/* Check threshold; if reached, invoke shutdown_trigger and return true to break
 * the loop. */
static bool trigger_shutdown(openups_ctx_t *restrict ctx) {
  if (ctx == NULL || ctx->consecutive_fails < ctx->config.fail_threshold) {
    return false;
  }

  const bool use_systemctl_poweroff =
      ctx->config.enable_systemd && ctx->systemd_enabled;
  shutdown_trigger(&ctx->config, &ctx->logger, use_systemctl_poweroff);

  if (ctx->config.shutdown_mode == SHUTDOWN_MODE_LOG_ONLY) {
    ctx->consecutive_fails =
        0; /* log-only: reset counter and keep monitoring */
    return false;
  }

  logger_info(&ctx->logger, "Shutdown triggered, exiting monitor loop");
  return true;
}

/* ============================================================ */
/* ============================================================ */
/* Reactor Engine                                               */
/* ============================================================ */

/*
 * Parse an incoming ICMP echo reply from the raw socket.
 * Returns 1 if a valid reply matching our (pid, sequence) was received,
 * filling out_result with latency information. Returns 0 if a packet
 * was read but should be ignored. Returns -1 if no more packets (EAGAIN/error).
 */
static int parse_icmp_reply(const openups_ctx_t *restrict ctx,
                            const struct sockaddr_storage *restrict dest_addr,
                            uint64_t now_ms, uint64_t send_time_ms,
                            ping_result_t *restrict out_result) {
  uint8_t recv_buf[4096] __attribute__((aligned(16)));
  struct sockaddr_storage recv_addr;
  socklen_t recv_addr_len = sizeof(recv_addr);

  ssize_t received = recvfrom(ctx->pinger.sockfd, recv_buf, sizeof(recv_buf), 0,
                              (struct sockaddr *)&recv_addr, &recv_addr_len);
  if (received < 0) {
    return -1;
  }
  if (received == 0 || recv_addr.ss_family != dest_addr->ss_family) {
    return 0;
  }

  /* Verify source address matches the target */
  if (dest_addr->ss_family == AF_INET) {
    const struct sockaddr_in *d = (const struct sockaddr_in *)dest_addr;
    const struct sockaddr_in *r = (const struct sockaddr_in *)&recv_addr;
    if (d->sin_addr.s_addr != r->sin_addr.s_addr)
      return 0;

    /* Validate minimum IP header size */
    if (received < (ssize_t)sizeof(struct ip))
      return 0;

    const struct ip *ip_hdr = (const struct ip *)recv_buf;
    if (ip_hdr->ip_p != IPPROTO_ICMP)
      return 0;

    size_t ip_hdr_len = (size_t)ip_hdr->ip_hl * 4;
    if (ip_hdr_len < sizeof(struct ip) || ip_hdr_len > (size_t)received)
      return 0;
    if (ip_hdr_len + sizeof(struct icmphdr) > (size_t)received)
      return 0;

    const struct icmphdr *recv_icmp =
        (const struct icmphdr *)(recv_buf + ip_hdr_len);

    if (recv_icmp->type != ICMP_ECHOREPLY)
      return 0;
    if (recv_icmp->un.echo.id != ctx->cached_pid)
      return 0;
    if (recv_icmp->un.echo.sequence != ctx->pinger.sequence)
      return 0;
  } else if (dest_addr->ss_family == AF_INET6) {
    const struct sockaddr_in6 *d = (const struct sockaddr_in6 *)dest_addr;
    const struct sockaddr_in6 *r = (const struct sockaddr_in6 *)&recv_addr;
    if (memcmp(&d->sin6_addr, &r->sin6_addr, sizeof(struct in6_addr)) != 0)
      return 0;

    /* For open IPv6 raw sockets, we only get the ICMPv6 header, no IP header */
    if (received < (ssize_t)sizeof(struct icmp6_hdr))
      return 0;

    const struct icmp6_hdr *recv_icmp6 = (const struct icmp6_hdr *)recv_buf;

    if (recv_icmp6->icmp6_type != ICMP6_ECHO_REPLY)
      return 0;
    if (recv_icmp6->icmp6_id != ctx->cached_pid)
      return 0;
    if (recv_icmp6->icmp6_seq != ctx->pinger.sequence)
      return 0;
  } else {
    return 0;
  }

  out_result->success = true;
  out_result->latency_ms = (double)(now_ms - send_time_ms);
  out_result->error_msg[0] = '\0';
  return 1;
}

static int openups_reactor_run(openups_ctx_t *restrict ctx) {
  if (ctx == NULL)
    return -1;
  const config_t *config = &ctx->config;

  /* Setup signalfd */
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGTERM);
  sigaddset(&mask, SIGUSR1);

  if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
    logger_error(&ctx->logger, "sigprocmask failed: %s", strerror(errno));
    return -1;
  }

  int sig_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
  if (sig_fd < 0) {
    logger_error(&ctx->logger, "signalfd failed: %s", strerror(errno));
    return -1;
  }

  uint64_t last_watchdog_ms = get_monotonic_ms();
  const uint16_t my_pid = ctx->cached_pid;

  logger_info(&ctx->logger,
              "Starting OpenUPS (Event Loop Engine) for target %s, every %ds",
              config->target, config->interval_sec);

  if (ctx->systemd_enabled) {
    (void)systemd_notifier_ready(&ctx->systemd);
    notify_systemd_status(ctx, "Monitoring %s (Event Loop)", config->target);
  }

  struct pollfd fds[2];
  fds[0].fd = sig_fd;
  fds[0].events = POLLIN;
  fds[1].fd = ctx->pinger.sockfd;
  fds[1].events = POLLIN;

  uint64_t now = get_monotonic_ms();
  uint64_t next_ping_ms = now;
  uint64_t reply_deadline_ms = 0;
  bool waiting_for_reply = false;
  uint64_t current_ping_send_time = 0;

  /* Calculate payload and packet length outside hot loop so ICMP size stays
   * constant */
  size_t header_len = (ctx->pinger.family == AF_INET6)
                          ? sizeof(struct icmp6_hdr)
                          : sizeof(struct icmphdr);
  /* The standard ping payload size makes the overall IP unfragmented packet at
   * 64 bytes */
  size_t packet_len = 64;
  size_t calculated_payload_size =
      packet_len > header_len ? packet_len - header_len : 0;
  if (packet_len > ctx->pinger.send_buf_capacity) {
    logger_error(&ctx->logger, "Buffer error: %zu exceeds capacity",
                 packet_len);
    return -1;
  }
  fill_payload_pattern(&ctx->pinger, header_len, calculated_payload_size);

  while (!ctx->stop_flag) {
    if (ctx->config.enable_watchdog && ctx->systemd_enabled) {
      if (now - last_watchdog_ms >= ctx->watchdog_interval_ms) {
        (void)systemd_notifier_watchdog(&ctx->systemd);
        last_watchdog_ms = now;
      }
    }

    /* 1: Check if a reply has timed out */
    if (waiting_for_reply && now >= reply_deadline_ms) {
      ping_result_t fail_res = {false, -1.0, "Timeout waiting for ICMP reply"};
      handle_ping_failure(ctx, &fail_res);
      waiting_for_reply = false;
      if (trigger_shutdown(ctx))
        break;
    }

    /* 2: Check if it is time to send the next ping */
    if (!waiting_for_reply && now >= next_ping_ms) {
      ctx->pinger.sequence = next_sequence(&ctx->pinger);

      if (ctx->pinger.family == AF_INET6) {
        struct icmp6_hdr *icmp6_hdr = (struct icmp6_hdr *)ctx->pinger.send_buf;
        memset(icmp6_hdr, 0, sizeof(*icmp6_hdr));
        icmp6_hdr->icmp6_type = ICMP6_ECHO_REQUEST;
        icmp6_hdr->icmp6_code = 0;
        icmp6_hdr->icmp6_id = my_pid;
        icmp6_hdr->icmp6_seq = ctx->pinger.sequence;
      } else {
        struct icmphdr *icmp_hdr = (struct icmphdr *)ctx->pinger.send_buf;
        memset(icmp_hdr, 0, sizeof(*icmp_hdr));
        icmp_hdr->type = ICMP_ECHO;
        icmp_hdr->code = 0;
        icmp_hdr->un.echo.id = my_pid;
        icmp_hdr->un.echo.sequence = ctx->pinger.sequence;
        icmp_hdr->checksum = 0;
        icmp_hdr->checksum =
            calculate_checksum(ctx->pinger.send_buf, packet_len);
      }

      ssize_t sent = sendto(
          ctx->pinger.sockfd, ctx->pinger.send_buf, packet_len, MSG_NOSIGNAL,
          (struct sockaddr *)&ctx->dest_addr, ctx->dest_addr_len);
      if (sent < 0) {
        ping_result_t fail_res = {false, -1.0, "Failed to send packet"};
        handle_ping_failure(ctx, &fail_res);
        if (trigger_shutdown(ctx))
          break;
      } else {
        current_ping_send_time = now;
        waiting_for_reply = true;
        reply_deadline_ms = now + (uint64_t)config->timeout_ms;
      }
      next_ping_ms += (uint64_t)config->interval_sec * 1000ULL;
      if (next_ping_ms < now)
        next_ping_ms = now + (uint64_t)config->interval_sec * 1000ULL;
    }

    /* 3: Compute exact timeout bounds for poll */
    int wait_timeout_ms = -1;

    if (waiting_for_reply) {
      wait_timeout_ms =
          reply_deadline_ms > now ? (int)(reply_deadline_ms - now) : 0;
    } else {
      wait_timeout_ms = next_ping_ms > now ? (int)(next_ping_ms - now) : 0;
    }

    if (ctx->config.enable_watchdog && ctx->systemd_enabled) {
      int wd_wait = (int)(last_watchdog_ms + ctx->watchdog_interval_ms - now);
      if (wait_timeout_ms == -1 || wd_wait < wait_timeout_ms) {
        wait_timeout_ms = wd_wait < 0 ? 0 : wd_wait;
      }
    }

    int rc = poll(fds, 2, wait_timeout_ms);
    if (rc < 0 && errno != EINTR) {
      logger_error(&ctx->logger, "poll error: %s", strerror(errno));
      break;
    }

    now = get_monotonic_ms();

    if (fds[0].revents & POLLIN) {
      struct signalfd_siginfo sfd_info;
      ssize_t res = read(sig_fd, &sfd_info, sizeof(struct signalfd_siginfo));
      if (res == sizeof(struct signalfd_siginfo)) {
        if (sfd_info.ssi_signo == SIGINT || sfd_info.ssi_signo == SIGTERM) {
          ctx->stop_flag = 1;
        } else if (sfd_info.ssi_signo == SIGUSR1) {
          openups_ctx_print_stats(ctx);
        }
      }
    }

    if (fds[1].revents & POLLIN) {
      ping_result_t reply;
      int parse_res;
      while ((parse_res = parse_icmp_reply(ctx, &ctx->dest_addr, now,
                                           current_ping_send_time, &reply)) !=
             -1) {
        if (parse_res == 1 && waiting_for_reply) {
          handle_ping_success(ctx, &reply);
          waiting_for_reply = false;
        }
      }
    }
  }

  if (ctx->stop_flag) {
    logger_info(&ctx->logger,
                "Received shutdown signal, stopping gracefully...");
    if (ctx->systemd_enabled) {
      (void)systemd_notifier_stopping(&ctx->systemd);
    }
  }

  openups_ctx_print_stats(ctx);
  logger_info(&ctx->logger, "OpenUPS monitor stopped");

  close(sig_fd);
  return 0;
}

int main(int argc, char **argv) {
  char error_msg[256];
  openups_ctx_t ctx;
  if (!openups_ctx_init(&ctx, argc, argv, error_msg, sizeof(error_msg))) {
    fprintf(stderr, "OpenUPS failed: %s\n", error_msg);
    return 1;
  }
  int rc = openups_reactor_run(&ctx);
  openups_ctx_destroy(&ctx);
  if (rc != 0) {
    fprintf(stderr, "OpenUPS exited with code %d\n", rc);
  }
  return rc;
}
