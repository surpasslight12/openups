#ifndef OPENUPS_MONITOR_RUNTIME_H
#define OPENUPS_MONITOR_RUNTIME_H

#include "monitor.h"
#include "monitor_state.h"

typedef enum {
  MONITOR_STEP_CONTINUE = 0,
  MONITOR_STEP_STOP = 1,
  MONITOR_STEP_ERROR = 2,
} monitor_step_result_t;

bool monitor_prepare_packet(openups_ctx_t *restrict ctx,
                            size_t *restrict packet_len);
void monitor_log_stats(openups_ctx_t *restrict ctx);
monitor_step_result_t monitor_handle_ping_timeout(
    openups_ctx_t *restrict ctx, monitor_state_t *restrict state,
    uint64_t now_ms);
monitor_step_result_t monitor_send_ping(openups_ctx_t *restrict ctx,
                                        monitor_state_t *restrict state,
                                        uint64_t now_ms,
                                        size_t packet_len);
monitor_step_result_t monitor_drain_icmp_replies(
    openups_ctx_t *restrict ctx, uint64_t now_ms,
    monitor_state_t *restrict state);

#endif