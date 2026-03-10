#include "openups.h"

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

typedef struct {
  uint64_t next_ping_ms;
  uint64_t reply_deadline_ms;
  uint64_t send_time_ms;
  uint64_t last_watchdog_ms;
  uint64_t shutdown_countdown_deadline_ms;
  uint16_t expected_sequence;
  bool waiting_for_reply;
  bool shutdown_countdown_pending;
} monitor_state_t;

typedef enum {
  MONITOR_STEP_CONTINUE = 0,
  MONITOR_STEP_STOP = 1,
  MONITOR_STEP_ERROR = 2,
} monitor_step_result_t;

static uint64_t monotonic_add_ms(uint64_t base_ms, uint64_t delta_ms) {
  uint64_t result = 0;
  if (OPENUPS_UNLIKELY(ckd_add(&result, base_ms, delta_ms))) {
    return UINT64_MAX;
  }
  return result;
}

static int deadline_timeout_ms(uint64_t now_ms, uint64_t deadline_ms) {
  if (deadline_ms <= now_ms) {
    return 0;
  }

  uint64_t remaining_ms = deadline_ms - now_ms;
  if (remaining_ms > (uint64_t)INT_MAX) {
    return INT_MAX;
  }

  return (int)remaining_ms;
}

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

static uint64_t config_delay_ms(const config_t *restrict config) {
  if (config == NULL) {
    return 0;
  }

  return config_duration_ms(config->delay_minutes, OPENUPS_MS_PER_MINUTE);
}

static uint64_t next_ping_deadline(uint64_t current_deadline_ms,
                                   uint64_t now_ms,
                                   uint64_t interval_ms) {
  uint64_t candidate_ms = monotonic_add_ms(current_deadline_ms, interval_ms);
  if (candidate_ms == UINT64_MAX || candidate_ms < now_ms) {
    return monotonic_add_ms(now_ms, interval_ms);
  }
  return candidate_ms;
}

static int compute_wait_timeout_ms(const openups_ctx_t *restrict ctx,
                                   const monitor_state_t *restrict state,
                                   uint64_t now_ms) {
  if (ctx == NULL || state == NULL) {
    return -1;
  }

  int timeout_ms = state->waiting_for_reply
                       ? deadline_timeout_ms(now_ms, state->reply_deadline_ms)
                       : deadline_timeout_ms(now_ms, state->next_ping_ms);

  if (state->shutdown_countdown_pending) {
    int delayed_shutdown_timeout_ms =
        deadline_timeout_ms(now_ms, state->shutdown_countdown_deadline_ms);
    if (delayed_shutdown_timeout_ms < timeout_ms) {
      timeout_ms = delayed_shutdown_timeout_ms;
    }
  }

  if (ctx->systemd_enabled && ctx->watchdog_interval_ms > 0) {
    uint64_t watchdog_deadline_ms =
        monotonic_add_ms(state->last_watchdog_ms, ctx->watchdog_interval_ms);
    int watchdog_timeout_ms = deadline_timeout_ms(now_ms, watchdog_deadline_ms);
    if (watchdog_deadline_ms == UINT64_MAX) {
      watchdog_timeout_ms = INT_MAX;
    }
    if (watchdog_timeout_ms < timeout_ms) {
      timeout_ms = watchdog_timeout_ms;
    }
  }

  return timeout_ms;
}

static void monitor_reply_cleared(monitor_state_t *restrict state) {
  if (state == NULL) {
    return;
  }

  state->waiting_for_reply = false;
  state->reply_deadline_ms = 0;
  state->send_time_ms = 0;
  state->expected_sequence = 0;
}

static void monitor_clear_shutdown_countdown(monitor_state_t *restrict state) {
  if (state == NULL) {
    return;
  }

  state->shutdown_countdown_pending = false;
  state->shutdown_countdown_deadline_ms = 0;
}

static bool pollfd_has_error(short revents) {
  return (revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
}

__attribute__((format(printf, 2, 3)))
static void notify_systemd_status(openups_ctx_t *restrict ctx,
                                  const char *restrict fmt, ...) {
  if (ctx == NULL || !ctx->systemd_enabled || fmt == NULL) {
    return;
  }

  char status_msg[OPENUPS_SYSTEMD_STATUS_SIZE];
  va_list args;
  va_start(args, fmt);
  vsnprintf(status_msg, sizeof(status_msg), fmt, args);
  va_end(args);

  (void)systemd_notifier_status(&ctx->systemd, status_msg);
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

static bool openups_ctx_init(openups_ctx_t *restrict ctx,
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
  ctx->systemd_enabled = systemd_notifier_is_enabled(&ctx->systemd);
  if (!ctx->systemd_enabled) {
    logger_debug(&ctx->logger, "systemd not detected, integration disabled");
    return true;
  }

  ctx->watchdog_interval_ms =
      systemd_notifier_watchdog_interval_ms(&ctx->systemd);
  logger_debug(&ctx->logger, "systemd integration enabled");
  if (ctx->watchdog_interval_ms > 0) {
    logger_debug(&ctx->logger, "watchdog interval: %" PRIu64 "ms",
                 ctx->watchdog_interval_ms);
  }

  return true;
}

static void openups_ctx_destroy(openups_ctx_t *restrict ctx) {
  if (ctx == NULL) {
    return;
  }

  icmp_pinger_destroy(&ctx->pinger);
  systemd_notifier_destroy(&ctx->systemd);
  memset(ctx, 0, sizeof(*ctx));
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

static void monitor_cancel_shutdown_countdown(openups_ctx_t *restrict ctx,
                                              monitor_state_t *restrict state) {
  if (ctx == NULL || state == NULL || !state->shutdown_countdown_pending) {
    return;
  }

  monitor_clear_shutdown_countdown(state);
  logger_info(&ctx->logger,
              "Connectivity restored; cancelled pending shutdown countdown");
  if (ctx->systemd_enabled) {
    notify_systemd_status(ctx,
                          "Recovery detected; shutdown countdown cancelled");
  }
}

static bool monitor_execute_shutdown(openups_ctx_t *restrict ctx) {
  if (ctx == NULL) {
    return false;
  }

  shutdown_result_t result =
      shutdown_trigger(&ctx->config, &ctx->logger, ctx->systemd_enabled);

  if (ctx->config.shutdown_mode == SHUTDOWN_MODE_LOG_ONLY) {
    ctx->consecutive_fails = 0;
    return false;
  }

  if (ctx->config.shutdown_mode == SHUTDOWN_MODE_DRY_RUN) {
    logger_info(&ctx->logger, "Shutdown triggered, exiting monitor loop");
    return true;
  }

  if (result != SHUTDOWN_RESULT_TRIGGERED) {
    logger_error(&ctx->logger,
                 "Shutdown command failed; continuing monitoring after reset");
    ctx->consecutive_fails = 0;
    return false;
  }

  logger_info(&ctx->logger, "Shutdown triggered, exiting monitor loop");
  return true;
}

static bool monitor_arm_shutdown_countdown(openups_ctx_t *restrict ctx,
                                           monitor_state_t *restrict state,
                                           uint64_t now_ms) {
  if (ctx == NULL || state == NULL) {
    return false;
  }

  if (state->shutdown_countdown_pending) {
    return false;
  }

  uint64_t delay_ms = config_delay_ms(&ctx->config);
  if (delay_ms == 0 || delay_ms == UINT64_MAX) {
    logger_error(&ctx->logger,
                 "Failed to compute delayed shutdown countdown duration");
    ctx->consecutive_fails = 0;
    return false;
  }

  uint64_t deadline_ms = monotonic_add_ms(now_ms, delay_ms);
  if (deadline_ms == UINT64_MAX) {
    logger_error(&ctx->logger,
                 "Failed to compute shutdown countdown deadline");
    ctx->consecutive_fails = 0;
    return false;
  }

  state->shutdown_countdown_pending = true;
  state->shutdown_countdown_deadline_ms = deadline_ms;
  log_shutdown_countdown(&ctx->logger, ctx->config.shutdown_mode,
                         ctx->config.delay_minutes);
  if (ctx->systemd_enabled) {
    notify_systemd_status(
        ctx, "%s countdown started: %d minutes",
        shutdown_mode_to_string(ctx->config.shutdown_mode),
        ctx->config.delay_minutes);
  }
  return false;
}

static bool monitor_handle_shutdown_countdown(openups_ctx_t *restrict ctx,
                                              monitor_state_t *restrict state,
                                              uint64_t now_ms) {
  if (ctx == NULL || state == NULL || !state->shutdown_countdown_pending) {
    return false;
  }

  if (now_ms < state->shutdown_countdown_deadline_ms) {
    return false;
  }

  monitor_clear_shutdown_countdown(state);
  logger_warn(&ctx->logger,
              "%s countdown elapsed; executing shutdown now",
              shutdown_mode_to_string(ctx->config.shutdown_mode));
  if (ctx->systemd_enabled) {
    notify_systemd_status(ctx, "%s countdown elapsed; executing shutdown",
                          shutdown_mode_to_string(ctx->config.shutdown_mode));
  }
  return monitor_execute_shutdown(ctx);
}

static void handle_ping_success(openups_ctx_t *restrict ctx,
                                monitor_state_t *restrict state,
                                const ping_result_t *restrict result) {
  if (ctx == NULL || state == NULL || result == NULL) {
    return;
  }

  ctx->consecutive_fails = 0;
  monitor_cancel_shutdown_countdown(ctx, state);
  metrics_record_success(&ctx->metrics, result->latency_ms);
  logger_debug(&ctx->logger, "Ping successful to %s, latency: %.2fms",
               ctx->config.target, result->latency_ms);

  if (ctx->systemd_enabled) {
    notify_systemd_status(
        ctx, "OK: %" PRIu64 "/%" PRIu64 " pings (%.1f%%), latency %.2fms",
        ctx->metrics.successful_pings, ctx->metrics.total_pings,
        metrics_success_rate(&ctx->metrics), result->latency_ms);
  }
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

  if (ctx->systemd_enabled) {
    notify_systemd_status(ctx,
                          "WARNING: %d consecutive failures, threshold is %d",
                          ctx->consecutive_fails, ctx->config.fail_threshold);
  }
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
  if (ctx->systemd_enabled) {
    notify_systemd_status(ctx, "ERROR: %s", error_msg);
  }

  return MONITOR_STEP_ERROR;
}

static bool monitor_handle_threshold(openups_ctx_t *restrict ctx,
                                     monitor_state_t *restrict state,
                                     uint64_t now_ms) {
  if (ctx == NULL || state == NULL ||
      ctx->consecutive_fails < ctx->config.fail_threshold) {
    return false;
  }

  if (ctx->config.shutdown_mode == SHUTDOWN_MODE_LOG_ONLY) {
    ctx->consecutive_fails = 0;
    return false;
  }

  if (ctx->config.delay_minutes > 0) {
    return monitor_arm_shutdown_countdown(ctx, state, now_ms);
  }

  return monitor_execute_shutdown(ctx);
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

  uint64_t reply_deadline_ms =
      monotonic_add_ms(now_ms, (uint64_t)ctx->config.timeout_ms);
  if (reply_deadline_ms == UINT64_MAX) {
    return monitor_runtime_error(ctx,
                                 "Failed to compute reply deadline");
  }

  state->waiting_for_reply = true;
  state->send_time_ms = now_ms;
  state->reply_deadline_ms = reply_deadline_ms;
  state->expected_sequence = ctx->pinger.sequence;
  return MONITOR_STEP_CONTINUE;
}

static bool drain_icmp_replies(openups_ctx_t *restrict ctx, uint64_t now_ms,
                               monitor_state_t *restrict state) {
  if (ctx == NULL || state == NULL) {
    return false;
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
        state->expected_sequence, state->send_time_ms, receive_time_ms,
        &reply);
    if (status == ICMP_RECEIVE_NO_MORE) {
      return true;
    }
    if (status == ICMP_RECEIVE_ERROR) {
      logger_warn(&ctx->logger, "Ignoring ICMP receive error: %s",
                  reply.error_msg);
      return true;
    }
    if (status == ICMP_RECEIVE_MATCHED && state->waiting_for_reply) {
      handle_ping_success(ctx, state, &reply);
      monitor_reply_cleared(state);
      return true;
    }
  }

  return true;
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

static int openups_reactor_run(openups_ctx_t *restrict ctx) {
  if (ctx == NULL) {
    return -1;
  }

  signal_channel_t signals;
  if (!signal_channel_init(&signals, &ctx->logger)) {
    return -1;
  }

  int exit_code = -1;
  size_t packet_len = 0;
  if (!monitor_prepare_packet(ctx, &packet_len)) {
    logger_error(&ctx->logger, "Failed to prepare ICMP packet buffer");
    goto cleanup;
  }

  uint64_t now_ms = get_monotonic_ms();
  const uint64_t interval_ms = config_interval_ms(&ctx->config);
  if (now_ms == UINT64_MAX || interval_ms == 0 || interval_ms == UINT64_MAX) {
    logger_error(&ctx->logger, "Failed to initialize monotonic timing state");
    goto cleanup;
  }

  monitor_state_t state = {
      .next_ping_ms = now_ms,
      .reply_deadline_ms = 0,
      .send_time_ms = 0,
      .last_watchdog_ms = now_ms,
        .shutdown_countdown_deadline_ms = 0,
      .expected_sequence = 0,
      .waiting_for_reply = false,
        .shutdown_countdown_pending = false,
  };

  logger_info(&ctx->logger, "Starting OpenUPS for target %s, every %ds",
              ctx->config.target, ctx->config.interval_sec);

  if (ctx->systemd_enabled) {
    if (!systemd_notifier_ready(&ctx->systemd)) {
      logger_warn(&ctx->logger,
                  "Failed to send systemd READY notification");
    }
    notify_systemd_status(ctx, "Monitoring %s", ctx->config.target);
  }

  struct pollfd fds[2] = {
      {.fd = signals.fd, .events = POLLIN, .revents = 0},
      {.fd = ctx->pinger.sockfd, .events = POLLIN, .revents = 0},
  };

  while (!ctx->stop_flag) {
    if (monitor_handle_shutdown_countdown(ctx, &state, now_ms)) {
      break;
    }

    if (ctx->systemd_enabled && ctx->watchdog_interval_ms > 0 &&
        now_ms - state.last_watchdog_ms >= ctx->watchdog_interval_ms) {
      if (systemd_notifier_watchdog(&ctx->systemd)) {
        state.last_watchdog_ms = now_ms;
      } else {
        logger_warn(&ctx->logger,
                    "Failed to send systemd WATCHDOG notification");
      }
    }

    if (state.waiting_for_reply && now_ms >= state.reply_deadline_ms) {
      ping_result_t timeout_result = {false, -1.0,
                                      "Timeout waiting for ICMP reply"};
      handle_ping_failure(ctx, &timeout_result);
      monitor_reply_cleared(&state);
      if (monitor_handle_threshold(ctx, &state, now_ms)) {
        break;
      }
    }

    if (!state.waiting_for_reply && now_ms >= state.next_ping_ms) {
      monitor_step_result_t send_result =
          monitor_send_ping(ctx, &state, now_ms, packet_len);
      if (send_result == MONITOR_STEP_ERROR) {
        exit_code = 1;
        break;
      }
      if (send_result == MONITOR_STEP_STOP) {
        break;
      }

      state.next_ping_ms =
          next_ping_deadline(state.next_ping_ms, now_ms, interval_ms);
      if (state.next_ping_ms == UINT64_MAX) {
        logger_error(&ctx->logger, "Failed to compute next ping deadline");
        break;
      }
    }

    int wait_timeout_ms = compute_wait_timeout_ms(ctx, &state, now_ms);
    int poll_result = poll(fds, 2, wait_timeout_ms);
    if (poll_result < 0 && errno != EINTR) {
      logger_error(&ctx->logger, "poll error: %s", strerror(errno));
      break;
    }

    uint64_t updated_now_ms = get_monotonic_ms();
    if (updated_now_ms != UINT64_MAX) {
      now_ms = updated_now_ms;
    }

    if (pollfd_has_error(fds[0].revents)) {
      logger_error(&ctx->logger, "Signal fd entered error state");
      break;
    }
    if (pollfd_has_error(fds[1].revents)) {
      logger_error(&ctx->logger, "ICMP socket entered error state");
      break;
    }

    if ((fds[0].revents & POLLIN) != 0) {
      monitor_handle_signal(ctx, &signals);
    }
    if ((fds[1].revents & POLLIN) != 0) {
      if (!drain_icmp_replies(ctx, now_ms, &state)) {
        break;
      }
    }

    fds[0].revents = 0;
    fds[1].revents = 0;
  }

  if (ctx->stop_flag) {
    logger_info(&ctx->logger, "Received shutdown signal, stopping gracefully...");
    if (ctx->systemd_enabled) {
      (void)systemd_notifier_stopping(&ctx->systemd);
    }
  }

  openups_ctx_print_stats(ctx);
  logger_info(&ctx->logger, "OpenUPS monitor stopped");
  exit_code = 0;

cleanup:
  signal_channel_destroy(&signals, &ctx->logger);
  return exit_code;
}

int main(int argc, char **argv) {
  char error_msg[256];
  bool exit_requested = false;
  config_t config;

  if (!config_resolve(&config, argc, argv, &exit_requested, error_msg,
                      sizeof(error_msg))) {
    fprintf(stderr, "OpenUPS failed: %s\n", error_msg);
    return 1;
  }

  if (exit_requested) {
    return 0;
  }

  openups_ctx_t ctx;
  if (!openups_ctx_init(&ctx, &config, error_msg, sizeof(error_msg))) {
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
