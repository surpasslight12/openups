#ifndef OPENUPS_SHUTDOWN_FSM_H
#define OPENUPS_SHUTDOWN_FSM_H

#include "monitor_state.h"

bool shutdown_fsm_cancel(openups_ctx_t *restrict ctx,
                         monitor_state_t *restrict state);
bool shutdown_fsm_handle_threshold(openups_ctx_t *restrict ctx,
                                   monitor_state_t *restrict state,
                                   uint64_t now_ms);
bool shutdown_fsm_handle_tick(openups_ctx_t *restrict ctx,
                              monitor_state_t *restrict state,
                              uint64_t now_ms);

#endif