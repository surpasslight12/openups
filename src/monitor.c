#include "monitor.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/signalfd.h>
#include <unistd.h>

/* ---- Types (was monitor_state.h + monitor_runtime.h) ---- */

typedef struct {
  uint64_t deadline_ms;
  uint64_t send_time_ms;
  uint16_t expected_sequence;
  bool waiting;
} monitor_ping_state_t;

typedef struct {
  uint64_t deadline_ms;
  bool pending;
} monitor_shutdown_state_t;

typedef struct {
  uint64_t next_ping_ms;
  uint64_t interval_ms;
} monitor_scheduler_state_t;

typedef struct {
  uint64_t last_sent_ms;
  uint64_t interval_ms;
} monitor_watchdog_state_t;

typedef struct {
  monitor_ping_state_t ping;
  monitor_shutdown_state_t shutdown;
  monitor_scheduler_state_t scheduler;
  monitor_watchdog_state_t watchdog;
} monitor_state_t;

typedef enum {
  MONITOR_STEP_CONTINUE = 0,
  MONITOR_STEP_STOP = 1,
  MONITOR_STEP_ERROR = 2,
} monitor_step_result_t;

#define OPENUPS_PACKET_SIZE 64U
#define OPENUPS_MAX_REPLY_DRAIN_PER_TICK 32U

/* ---- Metrics (was metrics.c) — static ---- */

static void metrics_init(metrics_t *metrics) {
  if (metrics == NULL) {
    return;
  }
  metrics->total_pings = 0;
  metrics->successful_pings = 0;
  metrics->failed_pings = 0;
  metrics->total_latency = 0.0;
  metrics->min_latency = -1.0;
  metrics->max_latency = -1.0;
  metrics->start_time_ms = get_monotonic_ms();
}

static void metrics_record_success(metrics_t *metrics, double latency_ms) {
  if (OPENUPS_UNLIKELY(metrics == NULL)) {
    return;
  }
  if (OPENUPS_UNLIKELY(latency_ms < 0.0)) {
    latency_ms = 0.0;
  }
  metrics->total_pings++;
  metrics->successful_pings++;
  metrics->total_latency += latency_ms;
  if (OPENUPS_UNLIKELY(metrics->min_latency < 0.0) ||
      latency_ms < metrics->min_latency) {
    metrics->min_latency = latency_ms;
  }
  if (OPENUPS_UNLIKELY(metrics->max_latency < 0.0) ||
      latency_ms > metrics->max_latency) {
    metrics->max_latency = latency_ms;
  }
}

static void metrics_record_failure(metrics_t *metrics) {
  if (OPENUPS_UNLIKELY(metrics == NULL)) {
    return;
  }
  metrics->total_pings++;
  metrics->failed_pings++;
}

static double metrics_success_rate(const metrics_t *metrics) {
  if (metrics == NULL || metrics->total_pings == 0) {
    return 0.0;
  }
  return (double)metrics->successful_pings / (double)metrics->total_pings *
         100.0;
}

static double metrics_avg_latency(const metrics_t *metrics) {
  if (metrics == NULL || metrics->successful_pings == 0) {
    return 0.0;
  }
  return metrics->total_latency / (double)metrics->successful_pings;
}

static uint64_t metrics_uptime_seconds(const metrics_t *metrics) {
  if (metrics == NULL) {
    return 0;
  }
  uint64_t now_ms = get_monotonic_ms();
  if (now_ms == UINT64_MAX || metrics->start_time_ms == UINT64_MAX ||
      now_ms < metrics->start_time_ms) {
    return 0;
  }
  return (now_ms - metrics->start_time_ms) / OPENUPS_MS_PER_SEC;
}

/* ---- Monitor state (was monitor_state.c) — static ---- */

static uint64_t monitor_deadline_add_ms(uint64_t base_ms, uint64_t delta_ms) {
  uint64_t result = 0;
  if (OPENUPS_UNLIKELY(ckd_add(&result, base_ms, delta_ms))) {
    return UINT64_MAX;
  }
  return result;
}

static int monitor_deadline_timeout_ms(uint64_t now_ms, uint64_t deadline_ms) {
  if (deadline_ms <= now_ms) {
    return 0;
  }
  uint64_t remaining_ms = deadline_ms - now_ms;
  if (remaining_ms > (uint64_t)INT_MAX) {
    return INT_MAX;
  }
  return (int)remaining_ms;
}

static int monitor_timeout_min(int lhs, int rhs) {
  return rhs < lhs ? rhs : lhs;
}

static void monitor_state_init(monitor_state_t *restrict state, uint64_t now_ms,
                               uint64_t interval_ms,
                               uint64_t watchdog_interval_ms) {
  if (state == NULL) {
    return;
  }
  memset(state, 0, sizeof(*state));
  state->scheduler.next_ping_ms = now_ms;
  state->scheduler.interval_ms = interval_ms;
  state->watchdog.last_sent_ms = now_ms;
  state->watchdog.interval_ms = watchdog_interval_ms;
}

static bool monitor_ping_arm(monitor_state_t *restrict state, uint64_t now_ms,
                             uint64_t timeout_ms, uint16_t expected_sequence) {
  if (state == NULL) {
    return false;
  }
  uint64_t deadline_ms = monitor_deadline_add_ms(now_ms, timeout_ms);
  if (deadline_ms == UINT64_MAX) {
    return false;
  }
  state->ping.waiting = true;
  state->ping.send_time_ms = now_ms;
  state->ping.deadline_ms = deadline_ms;
  state->ping.expected_sequence = expected_sequence;
  return true;
}

static void monitor_ping_clear(monitor_state_t *restrict state) {
  if (state == NULL) {
    return;
  }
  state->ping.waiting = false;
  state->ping.deadline_ms = 0;
  state->ping.send_time_ms = 0;
  state->ping.expected_sequence = 0;
}

static bool monitor_ping_waiting(const monitor_state_t *restrict state) {
  return state != NULL && state->ping.waiting;
}

static bool monitor_ping_deadline_elapsed(const monitor_state_t *restrict state,
                                          uint64_t now_ms) {
  return state != NULL && state->ping.waiting && now_ms >= state->ping.deadline_ms;
}

static uint64_t monitor_ping_send_time_ms(const monitor_state_t *restrict state) {
  return state != NULL ? state->ping.send_time_ms : 0;
}

static uint16_t monitor_ping_expected_sequence(
    const monitor_state_t *restrict state) {
  return state != NULL ? state->ping.expected_sequence : 0;
}

static bool monitor_shutdown_arm(monitor_state_t *restrict state,
                                 uint64_t now_ms, uint64_t delay_ms) {
  if (state == NULL) {
    return false;
  }
  uint64_t deadline_ms = monitor_deadline_add_ms(now_ms, delay_ms);
  if (deadline_ms == UINT64_MAX) {
    return false;
  }
  state->shutdown.pending = true;
  state->shutdown.deadline_ms = deadline_ms;
  return true;
}

static void monitor_shutdown_clear(monitor_state_t *restrict state) {
  if (state == NULL) {
    return;
  }
  state->shutdown.pending = false;
  state->shutdown.deadline_ms = 0;
}

static bool monitor_shutdown_pending(const monitor_state_t *restrict state) {
  return state != NULL && state->shutdown.pending;
}

static bool monitor_shutdown_deadline_elapsed(
    const monitor_state_t *restrict state, uint64_t now_ms) {
  return state != NULL && state->shutdown.pending &&
         now_ms >= state->shutdown.deadline_ms;
}

static bool monitor_scheduler_due(const monitor_state_t *restrict state,
                                  uint64_t now_ms) {
  return state != NULL && now_ms >= state->scheduler.next_ping_ms;
}

static bool monitor_scheduler_advance(monitor_state_t *restrict state,
                                      uint64_t now_ms) {
  if (state == NULL) {
    return false;
  }
  uint64_t candidate_ms = monitor_deadline_add_ms(state->scheduler.next_ping_ms,
                                                  state->scheduler.interval_ms);
  if (candidate_ms == UINT64_MAX || candidate_ms < now_ms) {
    candidate_ms = monitor_deadline_add_ms(now_ms, state->scheduler.interval_ms);
  }
  if (candidate_ms == UINT64_MAX) {
    return false;
  }
  state->scheduler.next_ping_ms = candidate_ms;
  return true;
}

static bool monitor_watchdog_due(const monitor_state_t *restrict state,
                                 uint64_t now_ms) {
  return state != NULL && state->watchdog.interval_ms > 0 &&
         now_ms - state->watchdog.last_sent_ms >= state->watchdog.interval_ms;
}

static void monitor_watchdog_mark_sent(monitor_state_t *restrict state,
                                       uint64_t now_ms) {
  if (state == NULL) {
    return;
  }
  state->watchdog.last_sent_ms = now_ms;
}

static int monitor_state_wait_timeout(const monitor_state_t *restrict state,
                                      uint64_t now_ms) {
  if (state == NULL) {
    return -1;
  }
  int timeout_ms = monitor_ping_waiting(state)
                       ? monitor_deadline_timeout_ms(now_ms, state->ping.deadline_ms)
                       : monitor_deadline_timeout_ms(now_ms,
                                                    state->scheduler.next_ping_ms);
  if (monitor_shutdown_pending(state)) {
    timeout_ms = monitor_timeout_min(
        timeout_ms,
        monitor_deadline_timeout_ms(now_ms, state->shutdown.deadline_ms));
  }
  if (state->watchdog.interval_ms > 0) {
    uint64_t watchdog_deadline_ms = monitor_deadline_add_ms(
        state->watchdog.last_sent_ms, state->watchdog.interval_ms);
    int watchdog_timeout_ms =
        watchdog_deadline_ms == UINT64_MAX
            ? INT_MAX
            : monitor_deadline_timeout_ms(now_ms, watchdog_deadline_ms);
    timeout_ms = monitor_timeout_min(timeout_ms, watchdog_timeout_ms);
  }
  return timeout_ms;
}

/* ---- Shutdown FSM (was shutdown_fsm.c) — static ---- */

static uint64_t shutdown_fsm_config_delay_ms(const config_t *restrict config) {
  if (config == NULL || config->delay_minutes <= 0) {
    return 0;
  }
  uint64_t delay_ms = 0;
  if (OPENUPS_UNLIKELY(ckd_mul(&delay_ms, (uint64_t)config->delay_minutes,
                               OPENUPS_MS_PER_MINUTE))) {
    return UINT64_MAX;
  }
  return delay_ms;
}

static void shutdown_fsm_reset_failures(openups_ctx_t *restrict ctx) {
  if (ctx == NULL) {
    return;
  }
  ctx->consecutive_fails = 0;
}

static bool shutdown_fsm_execute(openups_ctx_t *restrict ctx) {
  if (ctx == NULL) {
    return false;
  }
  shutdown_result_t result =
      shutdown_trigger(&ctx->config, &ctx->logger,
                       runtime_services_is_enabled(&ctx->services));
  if (ctx->config.shutdown_mode == SHUTDOWN_MODE_DRY_RUN) {
    logger_info(&ctx->logger, "Shutdown triggered, exiting monitor loop");
    return true;
  }
  if (result != SHUTDOWN_RESULT_TRIGGERED) {
    logger_error(&ctx->logger,
                 "Shutdown command failed; continuing monitoring with failure count preserved");
    return false;
  }
  logger_info(&ctx->logger, "Shutdown triggered, exiting monitor loop");
  return true;
}

static bool shutdown_fsm_cancel(openups_ctx_t *restrict ctx,
                                monitor_state_t *restrict state) {
  if (ctx == NULL || state == NULL || !monitor_shutdown_pending(state)) {
    return false;
  }
  monitor_shutdown_clear(state);
  logger_info(&ctx->logger,
              "Connectivity restored; cancelled pending shutdown countdown");
  (void)runtime_services_notify_statusf(
      &ctx->services, "Recovery detected; shutdown countdown cancelled");
  return true;
}

static bool shutdown_fsm_handle_threshold(openups_ctx_t *restrict ctx,
                                          monitor_state_t *restrict state,
                                          uint64_t now_ms) {
  if (ctx == NULL || state == NULL ||
      ctx->consecutive_fails < ctx->config.fail_threshold) {
    return false;
  }
  if (ctx->config.shutdown_mode == SHUTDOWN_MODE_LOG_ONLY) {
    shutdown_fsm_reset_failures(ctx);
    return false;
  }
  if (ctx->config.delay_minutes <= 0) {
    return shutdown_fsm_execute(ctx);
  }
  if (monitor_shutdown_pending(state)) {
    return false;
  }
  uint64_t delay_ms = shutdown_fsm_config_delay_ms(&ctx->config);
  if (delay_ms == 0 || delay_ms == UINT64_MAX) {
    logger_error(&ctx->logger,
                 "Failed to compute delayed shutdown countdown duration");
    shutdown_fsm_reset_failures(ctx);
    return false;
  }
  if (!monitor_shutdown_arm(state, now_ms, delay_ms)) {
    logger_error(&ctx->logger,
                 "Failed to compute shutdown countdown deadline");
    shutdown_fsm_reset_failures(ctx);
    return false;
  }
  log_shutdown_countdown(&ctx->logger, ctx->config.shutdown_mode,
                         ctx->config.delay_minutes);
  (void)runtime_services_notify_statusf(
      &ctx->services, "%s countdown started: %d minutes",
      shutdown_mode_to_string(ctx->config.shutdown_mode),
      ctx->config.delay_minutes);
  return false;
}

static bool shutdown_fsm_handle_tick(openups_ctx_t *restrict ctx,
                                     monitor_state_t *restrict state,
                                     uint64_t now_ms) {
  if (ctx == NULL || state == NULL || !monitor_shutdown_pending(state) ||
      !monitor_shutdown_deadline_elapsed(state, now_ms)) {
    return false;
  }
  monitor_shutdown_clear(state);
  logger_warn(&ctx->logger,
              "%s countdown elapsed; executing shutdown now",
              shutdown_mode_to_string(ctx->config.shutdown_mode));
  (void)runtime_services_notify_statusf(
      &ctx->services, "%s countdown elapsed; executing shutdown",
      shutdown_mode_to_string(ctx->config.shutdown_mode));
  return shutdown_fsm_execute(ctx);
}

/* ---- Runtime helpers (was monitor_runtime.c) — static ---- */

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
  (void)runtime_services_notify_statusf(
      &ctx->services, "OK: %" PRIu64 "/%" PRIu64 " pings (%.1f%%), latency %.2fms",
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
  logger_warn(&ctx->logger,
              "Ping failed to %s: %s (consecutive failures: %d)",
              ctx->config.target, result->error_msg, ctx->consecutive_fails);
  (void)runtime_services_notify_statusf(
      &ctx->services, "WARNING: %d consecutive failures, threshold is %d",
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
  (void)runtime_services_notify_statusf(&ctx->services, "ERROR: %s",
                                        error_msg);
  return MONITOR_STEP_ERROR;
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
  uint8_t *payload = ctx->pinger.send_buf + header_len;
  size_t payload_len = *packet_len - header_len;
  for (size_t i = 0; i < payload_len; i++) {
    payload[i] = (uint8_t)(i & 0xFFU);
  }
  return true;
}

static void monitor_log_stats(openups_ctx_t *restrict ctx) {
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

static monitor_step_result_t monitor_handle_ping_timeout(
    openups_ctx_t *restrict ctx, monitor_state_t *restrict state,
    uint64_t now_ms) {
  if (ctx == NULL || state == NULL ||
      !monitor_ping_deadline_elapsed(state, now_ms)) {
    return MONITOR_STEP_CONTINUE;
  }
  ping_result_t timeout_result = {false, 0.0, "ICMP reply deadline exceeded"};
  handle_ping_failure(ctx, &timeout_result);
  monitor_ping_clear(state);
  return shutdown_fsm_handle_threshold(ctx, state, now_ms)
             ? MONITOR_STEP_STOP
             : MONITOR_STEP_CONTINUE;
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

static monitor_step_result_t monitor_drain_icmp_replies(
    openups_ctx_t *restrict ctx, uint64_t now_ms,
    monitor_state_t *restrict state) {
  if (ctx == NULL || state == NULL) {
    return MONITOR_STEP_ERROR;
  }
  ping_result_t reply = {0};
  for (size_t processed = 0; processed < OPENUPS_MAX_REPLY_DRAIN_PER_TICK;
       processed++) {
    icmp_receive_status_t status = icmp_pinger_receive_reply(
        &ctx->pinger, &ctx->dest_addr, ctx->cached_pid,
        monitor_ping_expected_sequence(state), monitor_ping_send_time_ms(state),
        now_ms, &reply);
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

/* ---- Reactor (was original monitor.c) ---- */

typedef struct {
  int fd;
  sigset_t previous_mask;
  bool previous_mask_valid;
} signal_channel_t;

/* Renamed from monitor_runtime_t to avoid confusion with the merged module. */
typedef struct {
  signal_channel_t signals;
  monitor_state_t state;
  struct pollfd fds[2];
  size_t packet_len;
  uint64_t now_ms;
} monitor_loop_t;

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

static bool monitor_refresh_time(uint64_t *restrict now_ms) {
  if (now_ms == NULL) {
    return false;
  }
  uint64_t refreshed_now_ms = get_monotonic_ms();
  if (refreshed_now_ms == UINT64_MAX) {
    return false;
  }
  *now_ms = refreshed_now_ms;
  return true;
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
    monitor_log_stats(ctx);
  }
}

static bool monitor_notify_ready(openups_ctx_t *restrict ctx) {
  if (ctx == NULL || !runtime_services_is_enabled(&ctx->services)) {
    return true;
  }
  if (runtime_services_notify_ready(&ctx->services)) {
    return true;
  }
  logger_warn(&ctx->logger, "Failed to send systemd READY notification");
  return false;
}

static monitor_step_result_t monitor_handle_watchdog(
    openups_ctx_t *restrict ctx, monitor_state_t *restrict state,
    uint64_t now_ms) {
  if (ctx == NULL || state == NULL || !monitor_watchdog_due(state, now_ms)) {
    return MONITOR_STEP_CONTINUE;
  }
  if (runtime_services_notify_watchdog(&ctx->services)) {
    monitor_watchdog_mark_sent(state, now_ms);
    return MONITOR_STEP_CONTINUE;
  }
  logger_warn(&ctx->logger, "Failed to send systemd WATCHDOG notification");
  return MONITOR_STEP_CONTINUE;
}

static monitor_step_result_t monitor_handle_scheduler(
    openups_ctx_t *restrict ctx, monitor_state_t *restrict state,
    uint64_t now_ms, size_t packet_len) {
  if (ctx == NULL || state == NULL) {
    return MONITOR_STEP_ERROR;
  }
  if (monitor_ping_waiting(state) || !monitor_scheduler_due(state, now_ms)) {
    return MONITOR_STEP_CONTINUE;
  }
  monitor_step_result_t send_result =
      monitor_send_ping(ctx, state, now_ms, packet_len);
  if (send_result != MONITOR_STEP_CONTINUE) {
    return send_result;
  }
  if (!monitor_scheduler_advance(state, now_ms)) {
    logger_error(&ctx->logger, "Failed to compute next ping deadline");
    return MONITOR_STEP_ERROR;
  }
  return MONITOR_STEP_CONTINUE;
}

static monitor_step_result_t monitor_handle_poll_events(
    openups_ctx_t *restrict ctx, signal_channel_t *restrict signals,
    monitor_state_t *restrict state, struct pollfd fds[static 2],
    uint64_t *restrict now_ms) {
  if (ctx == NULL || signals == NULL || state == NULL || now_ms == NULL) {
    return MONITOR_STEP_ERROR;
  }
  int wait_timeout_ms = monitor_state_wait_timeout(state, *now_ms);
  int poll_result = poll(fds, 2, wait_timeout_ms);
  if (poll_result < 0 && errno != EINTR) {
    logger_error(&ctx->logger, "poll error: %s", strerror(errno));
    return MONITOR_STEP_ERROR;
  }
  (void)monitor_refresh_time(now_ms);
  if (pollfd_has_error(fds[0].revents)) {
    logger_error(&ctx->logger, "Signal fd entered error state");
    return MONITOR_STEP_ERROR;
  }
  if (pollfd_has_error(fds[1].revents)) {
    logger_error(&ctx->logger, "ICMP socket entered error state");
    return MONITOR_STEP_ERROR;
  }
  if ((fds[0].revents & POLLIN) != 0) {
    monitor_handle_signal(ctx, signals);
  }
  if ((fds[1].revents & POLLIN) != 0) {
    monitor_step_result_t receive_result =
        monitor_drain_icmp_replies(ctx, *now_ms, state);
    if (receive_result != MONITOR_STEP_CONTINUE) {
      return receive_result;
    }
  }
  fds[0].revents = 0;
  fds[1].revents = 0;
  return MONITOR_STEP_CONTINUE;
}

static bool monitor_loop_init(openups_ctx_t *restrict ctx,
                              monitor_loop_t *restrict loop) {
  if (ctx == NULL || loop == NULL) {
    return false;
  }
  memset(loop, 0, sizeof(*loop));
  loop->signals.fd = -1;
  if (!signal_channel_init(&loop->signals, &ctx->logger)) {
    return false;
  }
  if (!monitor_prepare_packet(ctx, &loop->packet_len)) {
    logger_error(&ctx->logger, "Failed to prepare ICMP packet buffer");
    signal_channel_destroy(&loop->signals, &ctx->logger);
    return false;
  }
  uint64_t interval_ms = config_interval_ms(&ctx->config);
  if (!monitor_refresh_time(&loop->now_ms) || interval_ms == 0 ||
      interval_ms == UINT64_MAX) {
    logger_error(&ctx->logger, "Failed to initialize monotonic timing state");
    signal_channel_destroy(&loop->signals, &ctx->logger);
    return false;
  }
  monitor_state_init(&loop->state, loop->now_ms, interval_ms,
                     runtime_services_watchdog_interval_ms(&ctx->services));
  loop->fds[0] = (struct pollfd){
      .fd = loop->signals.fd,
      .events = POLLIN,
      .revents = 0,
  };
  loop->fds[1] = (struct pollfd){
      .fd = ctx->pinger.sockfd,
      .events = POLLIN,
      .revents = 0,
  };
  return true;
}

static void monitor_loop_destroy(openups_ctx_t *restrict ctx,
                                 monitor_loop_t *restrict loop) {
  if (loop == NULL) {
    return;
  }
  signal_channel_destroy(&loop->signals, ctx != NULL ? &ctx->logger : NULL);
}

static monitor_step_result_t monitor_run_due_work(
    openups_ctx_t *restrict ctx, monitor_loop_t *restrict loop) {
  if (ctx == NULL || loop == NULL) {
    return MONITOR_STEP_ERROR;
  }
  if (shutdown_fsm_handle_tick(ctx, &loop->state, loop->now_ms)) {
    return MONITOR_STEP_STOP;
  }
  monitor_step_result_t step_result =
      monitor_handle_watchdog(ctx, &loop->state, loop->now_ms);
  if (step_result != MONITOR_STEP_CONTINUE) {
    return step_result;
  }
  step_result =
      monitor_handle_ping_timeout(ctx, &loop->state, loop->now_ms);
  if (step_result != MONITOR_STEP_CONTINUE) {
    return step_result;
  }
  return monitor_handle_scheduler(ctx, &loop->state, loop->now_ms,
                                  loop->packet_len);
}

static void monitor_log_startup(openups_ctx_t *restrict ctx) {
  if (ctx == NULL) {
    return;
  }
  logger_info(&ctx->logger, "Starting OpenUPS for target %s, every %ds",
              ctx->config.target, ctx->config.interval_sec);
  (void)monitor_notify_ready(ctx);
  (void)runtime_services_notify_statusf(&ctx->services, "Monitoring %s",
                                        ctx->config.target);
}

static void monitor_log_shutdown(openups_ctx_t *restrict ctx, int exit_code) {
  if (ctx == NULL) {
    return;
  }
  if (ctx->stop_flag) {
    logger_info(&ctx->logger,
                "Received shutdown signal, stopping gracefully...");
    if (runtime_services_is_enabled(&ctx->services)) {
      (void)runtime_services_notify_stopping(&ctx->services);
    }
  }
  monitor_log_stats(ctx);
  if (exit_code == OPENUPS_EXIT_SUCCESS) {
    logger_info(&ctx->logger, "OpenUPS monitor stopped");
  }
}

/* ---- Public API ---- */

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
  runtime_services_init(&ctx->services, &ctx->systemd,
                        ctx->config.enable_systemd);
  if (!runtime_services_is_enabled(&ctx->services)) {
    logger_debug(&ctx->logger, "systemd not detected, integration disabled");
    return true;
  }
  uint64_t watchdog_interval_ms =
      runtime_services_watchdog_interval_ms(&ctx->services);
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
  runtime_services_destroy(&ctx->services);
  icmp_pinger_destroy(&ctx->pinger);
  memset(ctx, 0, sizeof(*ctx));
}

int openups_reactor_run(openups_ctx_t *restrict ctx) {
  if (ctx == NULL) {
    return monitor_failure_exit_code();
  }
  monitor_loop_t loop;
  if (!monitor_loop_init(ctx, &loop)) {
    return monitor_failure_exit_code();
  }
  int exit_code = OPENUPS_EXIT_SUCCESS;
  monitor_log_startup(ctx);
  while (!ctx->stop_flag) {
    (void)monitor_refresh_time(&loop.now_ms);
    monitor_step_result_t step_result = monitor_run_due_work(ctx, &loop);
    if (step_result == MONITOR_STEP_ERROR) {
      exit_code = monitor_failure_exit_code();
      break;
    }
    if (step_result == MONITOR_STEP_STOP) {
      break;
    }
    step_result = monitor_handle_poll_events(ctx, &loop.signals,
                                             &loop.state, loop.fds,
                                             &loop.now_ms);
    if (step_result == MONITOR_STEP_ERROR) {
      exit_code = monitor_failure_exit_code();
      break;
    }
    if (step_result == MONITOR_STEP_STOP) {
      break;
    }
  }
  monitor_log_shutdown(ctx, exit_code);
  monitor_loop_destroy(ctx, &loop);
  return exit_code;
}
