#include "monitor_runtime.h"

#include "shutdown_fsm.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdarg.h>

#define OPENUPS_PACKET_SIZE 64U
#define OPENUPS_MAX_REPLY_DRAIN_PER_TICK 32U

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
  logger_warn(&ctx->logger,
              "Ping failed to %s: %s (consecutive failures: %d)",
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

bool monitor_prepare_packet(openups_ctx_t *restrict ctx,
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

void monitor_log_stats(openups_ctx_t *restrict ctx) {
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

monitor_step_result_t monitor_handle_ping_timeout(
    openups_ctx_t *restrict ctx, monitor_state_t *restrict state,
    uint64_t now_ms) {
  if (ctx == NULL || state == NULL || !monitor_ping_deadline_elapsed(state, now_ms)) {
    return MONITOR_STEP_CONTINUE;
  }

  ping_result_t timeout_result = {false, -1.0, "Timeout waiting for ICMP reply"};
  handle_ping_failure(ctx, &timeout_result);
  monitor_ping_clear(state);
  return shutdown_fsm_handle_threshold(ctx, state, now_ms) ? MONITOR_STEP_STOP
                                                            : MONITOR_STEP_CONTINUE;
}

monitor_step_result_t monitor_send_ping(openups_ctx_t *restrict ctx,
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

monitor_step_result_t monitor_drain_icmp_replies(
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
        receive_time_ms, &reply);
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