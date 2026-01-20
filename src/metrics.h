#ifndef OPENUPS_METRICS_H
#define OPENUPS_METRICS_H

#include <stdint.h>

/* 监控指标 */
typedef struct {
    uint64_t total_pings;
    uint64_t successful_pings;
    uint64_t failed_pings;
    double min_latency;
    double max_latency;
    double total_latency;
    uint64_t start_time_ms;
} metrics_t;

void metrics_init(metrics_t* metrics);
void metrics_record_success(metrics_t* metrics, double latency_ms);
void metrics_record_failure(metrics_t* metrics);

[[nodiscard]] double metrics_success_rate(const metrics_t* metrics);
[[nodiscard]] double metrics_avg_latency(const metrics_t* metrics);
[[nodiscard]] uint64_t metrics_uptime_seconds(const metrics_t* metrics);

#endif /* OPENUPS_METRICS_H */
