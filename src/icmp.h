#ifndef ICMP_H
#define ICMP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sys/socket.h>

typedef struct {
    bool success;
    double latency_ms;
    char error_msg[256];
} ping_result_t;

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

typedef void (*icmp_tick_fn)(void* user_data);
typedef bool (*icmp_should_stop_fn)(void* user_data);

[[nodiscard]] bool icmp_pinger_init(icmp_pinger_t* restrict pinger, bool use_ipv6,
                                    char* restrict error_msg, size_t error_size);
void icmp_pinger_destroy(icmp_pinger_t* restrict pinger);

[[nodiscard]] ping_result_t icmp_pinger_ping(icmp_pinger_t* restrict pinger,
                                             const char* restrict target, int timeout_ms,
                                             int payload_size);
[[nodiscard]] ping_result_t icmp_pinger_ping_ex(icmp_pinger_t* restrict pinger,
                                                const char* restrict target, int timeout_ms,
                                                int payload_size, icmp_tick_fn tick,
                                                void* tick_user_data,
                                                icmp_should_stop_fn should_stop,
                                                void* stop_user_data);

#endif /* ICMP_H */
