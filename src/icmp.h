#ifndef OPENUPS_ICMP_H
#define OPENUPS_ICMP_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Ping 结果 */
typedef struct {
    bool success;
    double latency_ms;
    char error_msg[256];
} ping_result_t;

/* ICMP Pinger 结构 */
typedef struct {
    bool use_ipv6;
    int sockfd;
} icmp_pinger_t;

/* 初始化 ICMP pinger */
[[nodiscard]] bool icmp_pinger_init(icmp_pinger_t* restrict pinger, bool use_ipv6, 
                                     char* restrict error_msg, size_t error_size);

/* 销毁 ICMP pinger */
void icmp_pinger_destroy(icmp_pinger_t* restrict pinger);

/* 执行 ping */
[[nodiscard]] ping_result_t icmp_pinger_ping(icmp_pinger_t* restrict pinger, 
                                             const char* restrict target, 
                                             int timeout_ms, int packet_size);

#endif /* OPENUPS_ICMP_H */
