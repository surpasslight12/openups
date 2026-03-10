#include "monitor.h"
#include "runtime_services.h"

#include <inttypes.h>
#include <string.h>
#include <unistd.h>

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