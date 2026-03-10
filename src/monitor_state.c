#include "monitor_state.h"

#include <limits.h>
#include <string.h>

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

void monitor_state_init(monitor_state_t *restrict state, uint64_t now_ms,
                        uint64_t interval_ms, uint64_t watchdog_interval_ms) {
  if (state == NULL) {
    return;
  }

  memset(state, 0, sizeof(*state));
  state->scheduler.next_ping_ms = now_ms;
  state->scheduler.interval_ms = interval_ms;
  state->watchdog.last_sent_ms = now_ms;
  state->watchdog.interval_ms = watchdog_interval_ms;
}

bool monitor_ping_arm(monitor_state_t *restrict state, uint64_t now_ms,
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

void monitor_ping_clear(monitor_state_t *restrict state) {
  if (state == NULL) {
    return;
  }

  state->ping.waiting = false;
  state->ping.deadline_ms = 0;
  state->ping.send_time_ms = 0;
  state->ping.expected_sequence = 0;
}

bool monitor_ping_waiting(const monitor_state_t *restrict state) {
  return state != NULL && state->ping.waiting;
}

bool monitor_ping_deadline_elapsed(const monitor_state_t *restrict state,
                                   uint64_t now_ms) {
  return state != NULL && state->ping.waiting && now_ms >= state->ping.deadline_ms;
}

uint64_t monitor_ping_send_time_ms(const monitor_state_t *restrict state) {
  return state != NULL ? state->ping.send_time_ms : 0;
}

uint16_t monitor_ping_expected_sequence(const monitor_state_t *restrict state) {
  return state != NULL ? state->ping.expected_sequence : 0;
}

bool monitor_shutdown_arm(monitor_state_t *restrict state, uint64_t now_ms,
                          uint64_t delay_ms) {
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

void monitor_shutdown_clear(monitor_state_t *restrict state) {
  if (state == NULL) {
    return;
  }

  state->shutdown.pending = false;
  state->shutdown.deadline_ms = 0;
}

bool monitor_shutdown_pending(const monitor_state_t *restrict state) {
  return state != NULL && state->shutdown.pending;
}

bool monitor_shutdown_deadline_elapsed(const monitor_state_t *restrict state,
                                       uint64_t now_ms) {
  return state != NULL && state->shutdown.pending &&
         now_ms >= state->shutdown.deadline_ms;
}

bool monitor_scheduler_due(const monitor_state_t *restrict state,
                           uint64_t now_ms) {
  return state != NULL && now_ms >= state->scheduler.next_ping_ms;
}

bool monitor_scheduler_advance(monitor_state_t *restrict state,
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

bool monitor_watchdog_due(const monitor_state_t *restrict state,
                          uint64_t now_ms) {
  return state != NULL && state->watchdog.interval_ms > 0 &&
         now_ms - state->watchdog.last_sent_ms >= state->watchdog.interval_ms;
}

void monitor_watchdog_mark_sent(monitor_state_t *restrict state,
                                uint64_t now_ms) {
  if (state == NULL) {
    return;
  }

  state->watchdog.last_sent_ms = now_ms;
}

int monitor_state_wait_timeout(const monitor_state_t *restrict state,
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