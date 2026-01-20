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

    /* 复用缓冲区与序列号（避免热路径 malloc/固定 seq） */
    uint16_t sequence;
    uint8_t* send_buf;
    size_t send_buf_capacity;
} icmp_pinger_t;

/* 回调：用于在等待回包期间执行周期性动作（例如喂 watchdog） */
typedef void (*icmp_tick_fn)(void* user_data);

/* 回调：用于在等待回包期间检查是否需要停止（例如响应 SIGTERM） */
typedef bool (*icmp_should_stop_fn)(void* user_data);

/* 初始化 ICMP pinger */
[[nodiscard]] bool icmp_pinger_init(icmp_pinger_t* restrict pinger, bool use_ipv6,
                                    char* restrict error_msg, size_t error_size);

/* 销毁 ICMP pinger */
void icmp_pinger_destroy(icmp_pinger_t* restrict pinger);

/* 执行 ping */
[[nodiscard]] ping_result_t icmp_pinger_ping(icmp_pinger_t* restrict pinger,
                                             const char* restrict target, int timeout_ms,
                                             int packet_size);

/* 执行 ping（扩展：等待期间可 tick/可被 stop 中断） */
[[nodiscard]] ping_result_t icmp_pinger_ping_ex(icmp_pinger_t* restrict pinger,
                                                const char* restrict target, int timeout_ms,
                                                int packet_size, icmp_tick_fn tick,
                                                void* tick_user_data,
                                                icmp_should_stop_fn should_stop,
                                                void* stop_user_data);

#endif /* OPENUPS_ICMP_H */
