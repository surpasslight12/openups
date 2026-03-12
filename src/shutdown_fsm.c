#include "shutdown_fsm.h"

#include <stdarg.h>

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

  if (ctx->config.shutdown_mode == SHUTDOWN_MODE_LOG_ONLY) {
    shutdown_fsm_reset_failures(ctx);
    return false;
  }

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

bool shutdown_fsm_cancel(openups_ctx_t *restrict ctx,
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

bool shutdown_fsm_handle_threshold(openups_ctx_t *restrict ctx,
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

bool shutdown_fsm_handle_tick(openups_ctx_t *restrict ctx,
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