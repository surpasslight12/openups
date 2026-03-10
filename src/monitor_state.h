#ifndef OPENUPS_MONITOR_STATE_H
#define OPENUPS_MONITOR_STATE_H

#include "openups.h"

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

void monitor_state_init(monitor_state_t *restrict state, uint64_t now_ms,
                        uint64_t interval_ms, uint64_t watchdog_interval_ms);

bool monitor_ping_arm(monitor_state_t *restrict state, uint64_t now_ms,
                      uint64_t timeout_ms, uint16_t expected_sequence);
void monitor_ping_clear(monitor_state_t *restrict state);
bool monitor_ping_waiting(const monitor_state_t *restrict state);
bool monitor_ping_deadline_elapsed(const monitor_state_t *restrict state,
                                   uint64_t now_ms);
uint64_t monitor_ping_send_time_ms(const monitor_state_t *restrict state);
uint16_t monitor_ping_expected_sequence(const monitor_state_t *restrict state);

bool monitor_shutdown_arm(monitor_state_t *restrict state, uint64_t now_ms,
                          uint64_t delay_ms);
void monitor_shutdown_clear(monitor_state_t *restrict state);
bool monitor_shutdown_pending(const monitor_state_t *restrict state);
bool monitor_shutdown_deadline_elapsed(const monitor_state_t *restrict state,
                                       uint64_t now_ms);

bool monitor_scheduler_due(const monitor_state_t *restrict state,
                           uint64_t now_ms);
bool monitor_scheduler_advance(monitor_state_t *restrict state,
                               uint64_t now_ms);

bool monitor_watchdog_due(const monitor_state_t *restrict state,
                          uint64_t now_ms);
void monitor_watchdog_mark_sent(monitor_state_t *restrict state,
                                uint64_t now_ms);

int monitor_state_wait_timeout(const monitor_state_t *restrict state,
                               uint64_t now_ms);

#endif