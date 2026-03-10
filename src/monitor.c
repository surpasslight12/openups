#include "monitor.h"
#include "monitor_state.h"
#include "runtime_services.h"
#include "shutdown_fsm.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/signalfd.h>
#include <unistd.h>

#define OPENUPS_PACKET_SIZE 64U
#define OPENUPS_MAX_REPLY_DRAIN_PER_TICK 32U

typedef struct {
  int fd;
  sigset_t previous_mask;
  bool previous_mask_valid;
} signal_channel_t;

typedef enum {
  MONITOR_STEP_CONTINUE = 0,
  MONITOR_STEP_STOP = 1,
  MONITOR_STEP_ERROR = 2,
} monitor_step_result_t;

static uint64_t config_duration_ms(int value, uint64_t scale_ms) {
  if (value <= 0) {
    return 0;
  }

  uint64_t duration_ms = 0;
  if (OPENUPS_UNLIKELY(ckd_mul(&duration_ms, (uint64_t)value, scale_ms))) {
    return UINT64_MAX;
  }

  return duration_ms;
}

static uint64_t config_interval_ms(const config_t *restrict config) {
  if (config == NULL) {
    return 0;
  }

  return config_duration_ms(config->interval_sec, OPENUPS_MS_PER_SEC);
}

static int monitor_failure_exit_code(void) {
  return OPENUPS_EXIT_FAILURE;
}

static bool pollfd_has_error(short revents) {
  return (revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
}

static bool signal_channel_init(signal_channel_t *restrict channel,
                                const logger_t *restrict logger) {
  if (channel == NULL || logger == NULL) {
    return false;
  }

  memset(channel, 0, sizeof(*channel));
  channel->fd = -1;

  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGTERM);
  sigaddset(&mask, SIGUSR1);

  if (sigprocmask(SIG_BLOCK, &mask, &channel->previous_mask) < 0) {
    logger_error(logger, "sigprocmask failed: %s", strerror(errno));
    return false;
  }
  channel->previous_mask_valid = true;

  channel->fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
  if (channel->fd < 0) {
    logger_error(logger, "signalfd failed: %s", strerror(errno));
    (void)sigprocmask(SIG_SETMASK, &channel->previous_mask, NULL);
    channel->previous_mask_valid = false;
    return false;
  }

  return true;
}

static void signal_channel_destroy(signal_channel_t *restrict channel,
                                   const logger_t *restrict logger) {
  if (channel == NULL) {
    return;
  }

  if (channel->fd >= 0) {
    close(channel->fd);
    channel->fd = -1;
  }

  if (channel->previous_mask_valid &&
      sigprocmask(SIG_SETMASK, &channel->previous_mask, NULL) < 0 &&
      logger != NULL) {
    logger_error(logger, "sigprocmask restore failed: %s", strerror(errno));
  }

  channel->previous_mask_valid = false;
}

static bool monitor_prepare_packet(openups_ctx_t *restrict ctx,
                                   size_t *restrict packet_len) {
  if (ctx == NULL || packet_len == NULL) {
    return false;
  }

  size_t header_len = (ctx->pinger.family == AF_INET6)
                          ? sizeof(struct icmp6_hdr)
                          : sizeof(struct icmphdr);
  *packet_len = OPENUPS_PACKET_SIZE;
  if (*packet_len > sizeof(ctx->pinger.send_buf) || header_len > *packet_len) {
    return false;
  }

  fill_payload_pattern(&ctx->pinger, header_len, *packet_len - header_len);
  return true;
}

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
                "%.2fms / avg %.2fms, uptime %" PRIu64 " seconds",
                metrics->total_pings, metrics->successful_pings,
                metrics->failed_pings, metrics_success_rate(metrics),
                metrics->min_latency, metrics->max_latency,
                metrics_avg_latency(metrics), metrics_uptime_seconds(metrics));
    return;
  }

  logger_info(&ctx->logger,
              "Statistics: %" PRIu64 " total pings, 0 successful, %" PRIu64
              " failed (0.00%% success rate), latency N/A, uptime %" PRIu64
              " seconds",
              metrics->total_pings, metrics->failed_pings,
              metrics_uptime_seconds(metrics));
}

static void handle_ping_success(openups_ctx_t *restrict ctx,
                                monitor_state_t *restrict state,
                                const ping_result_t *restrict result) {
  if (ctx == NULL || state == NULL || result == NULL) {
    return;
  }

  ctx->consecutive_fails = 0;
  (void)shutdown_fsm_cancel(ctx, state);
  metrics_record_success(&ctx->metrics, result->latency_ms);
  logger_debug(&ctx->logger, "Ping successful to %s, latency: %.2fms",
               ctx->config.target, result->latency_ms);

  runtime_services_notify_statusf(
      ctx, "OK: %" PRIu64 "/%" PRIu64 " pings (%.1f%%), latency %.2fms",
      ctx->metrics.successful_pings, ctx->metrics.total_pings,
      metrics_success_rate(&ctx->metrics), result->latency_ms);
}

static void handle_ping_failure(openups_ctx_t *restrict ctx,
                                const ping_result_t *restrict result) {
  if (ctx == NULL || result == NULL) {
    return;
  }

  ctx->consecutive_fails++;
  metrics_record_failure(&ctx->metrics);
  logger_warn(&ctx->logger, "Ping failed to %s: %s (consecutive failures: %d)",
              ctx->config.target, result->error_msg, ctx->consecutive_fails);

  runtime_services_notify_statusf(
      ctx, "WARNING: %d consecutive failures, threshold is %d",
      ctx->consecutive_fails, ctx->config.fail_threshold);
}

__attribute__((format(printf, 2, 3)))
static monitor_step_result_t monitor_runtime_error(
    openups_ctx_t *restrict ctx, const char *restrict fmt, ...) {
  if (ctx == NULL || fmt == NULL) {
    return MONITOR_STEP_ERROR;
  }

  char error_msg[OPENUPS_LOG_BUFFER_SIZE];
  va_list args;
  va_start(args, fmt);
  vsnprintf(error_msg, sizeof(error_msg), fmt, args);
  va_end(args);

  logger_error(&ctx->logger, "%s", error_msg);
  runtime_services_notify_statusf(ctx, "ERROR: %s", error_msg);

  return MONITOR_STEP_ERROR;
}

static monitor_step_result_t monitor_send_ping(openups_ctx_t *restrict ctx,
                                               monitor_state_t *restrict state,
                                               uint64_t now_ms,
                                               size_t packet_len) {
  if (ctx == NULL || state == NULL) {
    return MONITOR_STEP_ERROR;
  }

  ping_result_t error_result = {false, -1.0, {0}};
  if (!icmp_pinger_send_echo(&ctx->pinger, &ctx->dest_addr, ctx->dest_addr_len,
                             ctx->cached_pid, packet_len,
                             error_result.error_msg,
                             sizeof(error_result.error_msg))) {
    return monitor_runtime_error(ctx, "Failed to send ICMP echo: %s",
                                 error_result.error_msg);
  }

  if (!monitor_ping_arm(state, now_ms, (uint64_t)ctx->config.timeout_ms,
                        ctx->pinger.sequence)) {
    return monitor_runtime_error(ctx, "Failed to compute reply deadline");
  }
  return MONITOR_STEP_CONTINUE;
}

static monitor_step_result_t drain_icmp_replies(
    openups_ctx_t *restrict ctx, uint64_t now_ms,
    monitor_state_t *restrict state) {
  if (ctx == NULL || state == NULL) {
    return MONITOR_STEP_ERROR;
  }

  ping_result_t reply = {0};
  for (size_t processed = 0; processed < OPENUPS_MAX_REPLY_DRAIN_PER_TICK;
       processed++) {
    uint64_t receive_time_ms = get_monotonic_ms();
    if (receive_time_ms == UINT64_MAX) {
      receive_time_ms = now_ms;
    }

    icmp_receive_status_t status = icmp_pinger_receive_reply(
        &ctx->pinger, &ctx->dest_addr, ctx->cached_pid,
        monitor_ping_expected_sequence(state), monitor_ping_send_time_ms(state),
        receive_time_ms,
        &reply);
    if (status == ICMP_RECEIVE_NO_MORE) {
      return MONITOR_STEP_CONTINUE;
    }
    if (status == ICMP_RECEIVE_ERROR) {
      monitor_ping_clear(state);
      return monitor_runtime_error(ctx, "ICMP receive failed: %s",
                                   reply.error_msg);
    }
    if (status == ICMP_RECEIVE_MATCHED && monitor_ping_waiting(state)) {
      handle_ping_success(ctx, state, &reply);
      monitor_ping_clear(state);
      return MONITOR_STEP_CONTINUE;
    }
  }

  return MONITOR_STEP_CONTINUE;
}

static void monitor_handle_signal(openups_ctx_t *restrict ctx,
                                  const signal_channel_t *restrict signals) {
  if (ctx == NULL || signals == NULL || signals->fd < 0) {
    return;
  }

  struct signalfd_siginfo signal_info;
  ssize_t bytes = read(signals->fd, &signal_info, sizeof(signal_info));
  if (bytes != (ssize_t)sizeof(signal_info)) {
    return;
  }

  if (signal_info.ssi_signo == SIGINT || signal_info.ssi_signo == SIGTERM) {
    ctx->stop_flag = 1;
    return;
  }

  if (signal_info.ssi_signo == SIGUSR1) {
    openups_ctx_print_stats(ctx);
  }
}

bool openups_ctx_init(openups_ctx_t *restrict ctx,
                      const config_t *restrict config,
                      char *restrict error_msg, size_t error_size) {
  if (ctx == NULL || config == NULL || error_msg == NULL || error_size == 0) {
    return false;
  }

  memset(ctx, 0, sizeof(*ctx));
  ctx->config = *config;
  ctx->cached_pid = (uint16_t)(getpid() & 0xFFFF);
  if (ctx->cached_pid == 0) {
    ctx->cached_pid = 1;
  }

  logger_init(&ctx->logger, ctx->config.log_level,
              config_log_timestamps_enabled(&ctx->config));
  if (ctx->config.log_level == LOG_LEVEL_DEBUG) {
    config_print(&ctx->config, &ctx->logger);
  }

  ctx->dest_addr_len = sizeof(ctx->dest_addr);
  if (!resolve_target(ctx->config.target, &ctx->dest_addr, &ctx->dest_addr_len,
                      error_msg, error_size)) {
    return false;
  }

  int family = ((const struct sockaddr *)&ctx->dest_addr)->sa_family;
  if (!icmp_pinger_init(&ctx->pinger, family, error_msg, error_size)) {
    return false;
  }

  metrics_init(&ctx->metrics);

  if (!ctx->config.enable_systemd) {
    return true;
  }

  systemd_notifier_init(&ctx->systemd);
  if (!runtime_services_systemd_enabled(ctx)) {
    logger_debug(&ctx->logger, "systemd not detected, integration disabled");
    return true;
  }

  uint64_t watchdog_interval_ms = runtime_services_watchdog_interval_ms(ctx);
  logger_debug(&ctx->logger, "systemd integration enabled");
  if (watchdog_interval_ms > 0) {
    logger_debug(&ctx->logger, "watchdog interval: %" PRIu64 "ms",
                 watchdog_interval_ms);
  }

  return true;
}

void openups_ctx_destroy(openups_ctx_t *restrict ctx) {
  if (ctx == NULL) {
    return;
  }

  icmp_pinger_destroy(&ctx->pinger);
  systemd_notifier_destroy(&ctx->systemd);
  memset(ctx, 0, sizeof(*ctx));
}

int openups_reactor_run(openups_ctx_t *restrict ctx) {
  if (ctx == NULL) {
    return monitor_failure_exit_code();
  }

  signal_channel_t signals;
  if (!signal_channel_init(&signals, &ctx->logger)) {
    return monitor_failure_exit_code();
  }

  int exit_code = OPENUPS_EXIT_SUCCESS;
  size_t packet_len = 0;
  if (!monitor_prepare_packet(ctx, &packet_len)) {
    logger_error(&ctx->logger, "Failed to prepare ICMP packet buffer");
    exit_code = monitor_failure_exit_code();
    goto cleanup;
  }

  uint64_t now_ms = get_monotonic_ms();
  const uint64_t interval_ms = config_interval_ms(&ctx->config);
  if (now_ms == UINT64_MAX || interval_ms == 0 || interval_ms == UINT64_MAX) {
    logger_error(&ctx->logger, "Failed to initialize monotonic timing state");
    exit_code = monitor_failure_exit_code();
    goto cleanup;
  }

  monitor_state_t state;
  monitor_state_init(&state, now_ms, interval_ms,
                     runtime_services_watchdog_interval_ms(ctx));

  logger_info(&ctx->logger, "Starting OpenUPS for target %s, every %ds",
              ctx->config.target, ctx->config.interval_sec);

  if (runtime_services_systemd_enabled(ctx)) {
    if (!runtime_services_notify_ready(ctx)) {
      logger_warn(&ctx->logger, "Failed to send systemd READY notification");
    }
  }
  runtime_services_notify_statusf(ctx, "Monitoring %s", ctx->config.target);

  struct pollfd fds[2] = {
      {.fd = signals.fd, .events = POLLIN, .revents = 0},
      {.fd = ctx->pinger.sockfd, .events = POLLIN, .revents = 0},
  };

  while (!ctx->stop_flag) {
    uint64_t refreshed_now_ms = get_monotonic_ms();
    if (refreshed_now_ms != UINT64_MAX) {
      now_ms = refreshed_now_ms;
    }

    if (shutdown_fsm_handle_tick(ctx, &state, now_ms)) {
      break;
    }

    if (monitor_watchdog_due(&state, now_ms)) {
      if (runtime_services_notify_watchdog(ctx)) {
        monitor_watchdog_mark_sent(&state, now_ms);
      } else {
        logger_warn(&ctx->logger,
                    "Failed to send systemd WATCHDOG notification");
      }
    }

    if (monitor_ping_deadline_elapsed(&state, now_ms)) {
      ping_result_t timeout_result = {false, -1.0,
                                      "Timeout waiting for ICMP reply"};
      handle_ping_failure(ctx, &timeout_result);
      monitor_ping_clear(&state);
      if (shutdown_fsm_handle_threshold(ctx, &state, now_ms)) {
        break;
      }
    }

    if (!monitor_ping_waiting(&state) && monitor_scheduler_due(&state, now_ms)) {
      monitor_step_result_t send_result =
          monitor_send_ping(ctx, &state, now_ms, packet_len);
      if (send_result == MONITOR_STEP_ERROR) {
        exit_code = monitor_failure_exit_code();
        break;
      }
      if (send_result == MONITOR_STEP_STOP) {
        break;
      }

      if (!monitor_scheduler_advance(&state, now_ms)) {
        logger_error(&ctx->logger, "Failed to compute next ping deadline");
        exit_code = monitor_failure_exit_code();
        break;
      }
    }

    int wait_timeout_ms = monitor_state_wait_timeout(&state, now_ms);
    int poll_result = poll(fds, 2, wait_timeout_ms);
    if (poll_result < 0 && errno != EINTR) {
      logger_error(&ctx->logger, "poll error: %s", strerror(errno));
      exit_code = monitor_failure_exit_code();
      break;
    }

    uint64_t updated_now_ms = get_monotonic_ms();
    if (updated_now_ms != UINT64_MAX) {
      now_ms = updated_now_ms;
    }

    if (pollfd_has_error(fds[0].revents)) {
      logger_error(&ctx->logger, "Signal fd entered error state");
      exit_code = monitor_failure_exit_code();
      break;
    }
    if (pollfd_has_error(fds[1].revents)) {
      logger_error(&ctx->logger, "ICMP socket entered error state");
      exit_code = monitor_failure_exit_code();
      break;
    }

    if ((fds[0].revents & POLLIN) != 0) {
      monitor_handle_signal(ctx, &signals);
    }
    if ((fds[1].revents & POLLIN) != 0) {
      monitor_step_result_t receive_result =
          drain_icmp_replies(ctx, now_ms, &state);
      if (receive_result == MONITOR_STEP_ERROR) {
        exit_code = monitor_failure_exit_code();
        break;
      }
      if (receive_result == MONITOR_STEP_STOP) {
        break;
      }
    }

    fds[0].revents = 0;
    fds[1].revents = 0;
  }

  if (ctx->stop_flag) {
    logger_info(&ctx->logger,
                "Received shutdown signal, stopping gracefully...");
    if (runtime_services_systemd_enabled(ctx)) {
      (void)runtime_services_notify_stopping(ctx);
    }
  }

  openups_ctx_print_stats(ctx);
  if (exit_code == OPENUPS_EXIT_SUCCESS) {
    logger_info(&ctx->logger, "OpenUPS monitor stopped");
  }

cleanup:
  signal_channel_destroy(&signals, &ctx->logger);
  return exit_code;
}