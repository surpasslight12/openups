#include "monitor.h"

#include "monitor_runtime.h"
#include "monitor_state.h"
#include "shutdown_fsm.h"

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <string.h>
#include <sys/signalfd.h>
#include <unistd.h>

typedef struct {
  int fd;
  sigset_t previous_mask;
  bool previous_mask_valid;
} signal_channel_t;

typedef struct {
  signal_channel_t signals;
  monitor_state_t state;
  struct pollfd fds[2];
  size_t packet_len;
  uint64_t now_ms;
} monitor_runtime_t;

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

static bool monitor_runtime_init(openups_ctx_t *restrict ctx,
                                 monitor_runtime_t *restrict runtime) {
  if (ctx == NULL || runtime == NULL) {
    return false;
  }

  memset(runtime, 0, sizeof(*runtime));
  runtime->signals.fd = -1;

  if (!signal_channel_init(&runtime->signals, &ctx->logger)) {
    return false;
  }

  if (!monitor_prepare_packet(ctx, &runtime->packet_len)) {
    logger_error(&ctx->logger, "Failed to prepare ICMP packet buffer");
    signal_channel_destroy(&runtime->signals, &ctx->logger);
    return false;
  }

  uint64_t interval_ms = config_interval_ms(&ctx->config);
  if (!monitor_refresh_time(&runtime->now_ms) || interval_ms == 0 ||
      interval_ms == UINT64_MAX) {
    logger_error(&ctx->logger, "Failed to initialize monotonic timing state");
    signal_channel_destroy(&runtime->signals, &ctx->logger);
    return false;
  }

  monitor_state_init(&runtime->state, runtime->now_ms, interval_ms,
                     runtime_services_watchdog_interval_ms(&ctx->services));
  runtime->fds[0] = (struct pollfd){
      .fd = runtime->signals.fd,
      .events = POLLIN,
      .revents = 0,
  };
  runtime->fds[1] = (struct pollfd){
      .fd = ctx->pinger.sockfd,
      .events = POLLIN,
      .revents = 0,
  };

  return true;
}

static void monitor_runtime_destroy(openups_ctx_t *restrict ctx,
                                    monitor_runtime_t *restrict runtime) {
  if (runtime == NULL) {
    return;
  }

  signal_channel_destroy(&runtime->signals,
                         ctx != NULL ? &ctx->logger : NULL);
}

static monitor_step_result_t monitor_run_due_work(
    openups_ctx_t *restrict ctx, monitor_runtime_t *restrict runtime) {
  if (ctx == NULL || runtime == NULL) {
    return MONITOR_STEP_ERROR;
  }

  if (shutdown_fsm_handle_tick(ctx, &runtime->state, runtime->now_ms)) {
    return MONITOR_STEP_STOP;
  }

  monitor_step_result_t step_result =
      monitor_handle_watchdog(ctx, &runtime->state, runtime->now_ms);
  if (step_result != MONITOR_STEP_CONTINUE) {
    return step_result;
  }

  step_result =
      monitor_handle_ping_timeout(ctx, &runtime->state, runtime->now_ms);
  if (step_result != MONITOR_STEP_CONTINUE) {
    return step_result;
  }

  return monitor_handle_scheduler(ctx, &runtime->state, runtime->now_ms,
                                  runtime->packet_len);
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

  monitor_runtime_t runtime;
  if (!monitor_runtime_init(ctx, &runtime)) {
    return monitor_failure_exit_code();
  }

  int exit_code = OPENUPS_EXIT_SUCCESS;
  monitor_log_startup(ctx);

  while (!ctx->stop_flag) {
    (void)monitor_refresh_time(&runtime.now_ms);

    monitor_step_result_t step_result = monitor_run_due_work(ctx, &runtime);
    if (step_result == MONITOR_STEP_ERROR) {
      exit_code = monitor_failure_exit_code();
      break;
    }
    if (step_result == MONITOR_STEP_STOP) {
      break;
    }

    step_result = monitor_handle_poll_events(ctx, &runtime.signals,
                                             &runtime.state, runtime.fds,
                                             &runtime.now_ms);
    if (step_result == MONITOR_STEP_ERROR) {
      exit_code = monitor_failure_exit_code();
      break;
    }
    if (step_result == MONITOR_STEP_STOP) {
      break;
    }
  }

  monitor_log_shutdown(ctx, exit_code);
  monitor_runtime_destroy(ctx, &runtime);
  return exit_code;
}