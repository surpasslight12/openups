#ifndef OPENUPS_ICMP_H
#define OPENUPS_ICMP_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h>

/* 单次 ping 结果。 */
typedef struct {
    bool success;
    double latency_ms;
    char error_msg[256];
} ping_result_t;

/* ICMP pinger（IPv4/IPv6 raw socket）。 */
typedef struct {
    bool use_ipv6;
    int sockfd;

    uint16_t sequence;
    uint8_t* send_buf;
    size_t send_buf_capacity;
    size_t payload_filled_size;

    bool cached_target_valid;
    char cached_target[256];
    struct sockaddr_storage cached_addr;
    socklen_t cached_addr_len;
} icmp_pinger_t;

/* 等待回包期间的 tick 回调（可用于 watchdog 心跳等）。 */
typedef void (*icmp_tick_fn)(void* user_data);

/* 等待回包期间的停止检查回调（用于中断等待）。 */
typedef bool (*icmp_should_stop_fn)(void* user_data);

/* 初始化 ICMP pinger；失败时写入 error_msg。 */
[[nodiscard]] bool icmp_pinger_init(icmp_pinger_t* restrict pinger, bool use_ipv6,
                                    char* restrict error_msg, size_t error_size);

/* 销毁 ICMP pinger。 */
void icmp_pinger_destroy(icmp_pinger_t* restrict pinger);

/* 执行 ping。 */
[[nodiscard]] ping_result_t icmp_pinger_ping(icmp_pinger_t* restrict pinger,
                                             const char* restrict target, int timeout_ms,
                                             int packet_size);

/* 执行 ping（扩展：等待期间可 tick/可被 stop 中断）。 */
[[nodiscard]] ping_result_t icmp_pinger_ping_ex(icmp_pinger_t* restrict pinger,
                                                const char* restrict target, int timeout_ms,
                                                int packet_size, icmp_tick_fn tick,
                                                void* tick_user_data,
                                                icmp_should_stop_fn should_stop,
                                                void* stop_user_data);

#endif /* OPENUPS_ICMP_H */
