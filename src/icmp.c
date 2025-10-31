/* 必须在包含任何头文件之前定义 BSD 宏 */
#define __USE_BSD
#define __FAVOR_BSD

#include "icmp.h"
#include "common.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/icmp6.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* 编译时检查 ICMP 结构体大小 */
static_assert(sizeof(struct icmphdr) >= 8, "icmphdr must be at least 8 bytes");
static_assert(sizeof(struct icmp6_hdr) >= 8, "icmp6_hdr must be at least 8 bytes");

/* ICMP 校验和计算 */
static uint16_t calculate_checksum(const void* data, size_t len) {
    const uint16_t* buf = (const uint16_t*)data;
    uint32_t sum = 0;
    
    /* 累加所有 16 位字 */
    for (size_t i = 0; i < len / 2; i++) {
        sum += buf[i];
    }
    
    /* 如果长度是奇数，添加最后一个字节 */
    if (len % 2) {
        sum += ((const uint8_t*)data)[len - 1];
    }
    
    /* 将进位加回低 16 位 */
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    
    return ~sum;
}

bool icmp_pinger_init(icmp_pinger_t* restrict pinger, bool use_ipv6, 
                      char* restrict error_msg, size_t error_size) {
    if (pinger == nullptr || error_msg == nullptr || error_size == 0) {
        return false;
    }
    
    pinger->use_ipv6 = use_ipv6;
    
    /* 创建 raw socket */
    int family = use_ipv6 ? AF_INET6 : AF_INET;
    int protocol = use_ipv6 ? IPPROTO_ICMPV6 : IPPROTO_ICMP;
    
    pinger->sockfd = socket(family, SOCK_RAW, protocol);
    if (pinger->sockfd < 0) {
        snprintf(error_msg, error_size, 
                "Failed to create socket: %s (需要 root 权限或 CAP_NET_RAW)", 
                strerror(errno));
        return false;
    }
    
    return true;
}

void icmp_pinger_destroy(icmp_pinger_t* restrict pinger) {
    if (pinger == nullptr) {
        return;
    }
    
    if (pinger->sockfd >= 0) {
        close(pinger->sockfd);
        pinger->sockfd = -1;
    }
}

/* 解析目标地址 */
static bool resolve_target(const char* restrict target, bool use_ipv6, 
                          struct sockaddr_storage* restrict addr, socklen_t* restrict addr_len,
                          char* restrict error_msg, size_t error_size) {
    if (target == nullptr || addr == nullptr || addr_len == nullptr || 
        error_msg == nullptr || error_size == 0) {
        return false;
    }
    
    struct addrinfo hints, *result;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = use_ipv6 ? AF_INET6 : AF_INET;
    hints.ai_socktype = SOCK_RAW;
    
    int ret = getaddrinfo(target, nullptr, &hints, &result);
    if (ret != 0) {
        snprintf(error_msg, error_size, "Failed to resolve target: %s", 
                gai_strerror(ret));
        return false;
    }
    
    memcpy(addr, result->ai_addr, result->ai_addrlen);
    *addr_len = result->ai_addrlen;
    
    freeaddrinfo(result);
    return true;
}

/* IPv4 Ping 实现 */
static ping_result_t ping_ipv4(icmp_pinger_t* restrict pinger, struct sockaddr_in* restrict dest_addr,
                               int timeout_ms, int packet_size) {
    ping_result_t result = {false, 0.0, ""};
    
    if (pinger == nullptr || dest_addr == nullptr) {
        snprintf(result.error_msg, sizeof(result.error_msg), "Invalid arguments");
        return result;
    }
    
    /* 构建 ICMP Echo Request */
    size_t packet_len = sizeof(struct icmphdr) + packet_size;
    uint8_t* packet = (uint8_t*)calloc(1, packet_len);
    if (packet == nullptr) {
        snprintf(result.error_msg, sizeof(result.error_msg), "Memory allocation failed");
        return result;
    }
    
    struct icmphdr* icmp_hdr = (struct icmphdr*)packet;
    icmp_hdr->type = ICMP_ECHO;
    icmp_hdr->code = 0;
    icmp_hdr->un.echo.id = getpid() & 0xFFFF;
    icmp_hdr->un.echo.sequence = 1;
    
    /* 填充数据 */
    for (int i = 0; i < packet_size; i++) {
        packet[sizeof(struct icmphdr) + i] = i & 0xFF;
    }
    
    /* 计算校验和 */
    icmp_hdr->checksum = 0;
    icmp_hdr->checksum = calculate_checksum(packet, packet_len);
    
    /* 设置超时 */
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(pinger->sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    /* 发送 ICMP Echo Request */
    struct timeval send_time;
    gettimeofday(&send_time, nullptr);
    
    ssize_t sent = sendto(pinger->sockfd, packet, packet_len, 0,
                          (struct sockaddr*)dest_addr, sizeof(*dest_addr));
    if (sent < 0) {
        snprintf(result.error_msg, sizeof(result.error_msg), 
                "Failed to send packet: %s", strerror(errno));
        free(packet);
        return result;
    }
    
    /* 接收 ICMP Echo Reply */
    uint8_t recv_buf[4096];
    struct sockaddr_in recv_addr;
    socklen_t recv_addr_len = sizeof(recv_addr);
    
    ssize_t received = recvfrom(pinger->sockfd, recv_buf, sizeof(recv_buf), 0,
                               (struct sockaddr*)&recv_addr, &recv_addr_len);
    
    struct timeval recv_time;
    gettimeofday(&recv_time, nullptr);
    
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            snprintf(result.error_msg, sizeof(result.error_msg), "Timeout");
        } else {
            snprintf(result.error_msg, sizeof(result.error_msg), 
                    "Failed to receive: %s", strerror(errno));
        }
        free(packet);
        return result;
    }
    
    /* 解析 IP 头和 ICMP 头 */
    struct ip* ip_hdr = (struct ip*)recv_buf;
    size_t ip_hdr_len = ip_hdr->ip_hl * 4;
    struct icmphdr* recv_icmp = (struct icmphdr*)(recv_buf + ip_hdr_len);
    
    /* 验证 ICMP Echo Reply */
    if (recv_icmp->type == ICMP_ECHOREPLY &&
        recv_icmp->un.echo.id == (getpid() & 0xFFFF)) {
        
        /* 计算延迟 */
        double latency = (recv_time.tv_sec - send_time.tv_sec) * 1000.0 +
                        (recv_time.tv_usec - send_time.tv_usec) / 1000.0;
        
        result.success = true;
        result.latency_ms = latency;
    } else {
        snprintf(result.error_msg, sizeof(result.error_msg), 
                "Invalid ICMP reply (type=%d)", recv_icmp->type);
    }
    
    free(packet);
    return result;
}

/* IPv6 Ping 实现 */
static ping_result_t ping_ipv6(icmp_pinger_t* restrict pinger, struct sockaddr_in6* restrict dest_addr,
                               int timeout_ms, int packet_size) {
    ping_result_t result = {false, 0.0, ""};
    
    if (pinger == nullptr || dest_addr == nullptr) {
        snprintf(result.error_msg, sizeof(result.error_msg), "Invalid arguments");
        return result;
    }
    
    /* 构建 ICMPv6 Echo Request */
    size_t packet_len = sizeof(struct icmp6_hdr) + packet_size;
    uint8_t* packet = (uint8_t*)calloc(1, packet_len);
    if (packet == nullptr) {
        snprintf(result.error_msg, sizeof(result.error_msg), "Memory allocation failed");
        return result;
    }
    
    struct icmp6_hdr* icmp6_hdr = (struct icmp6_hdr*)packet;
    icmp6_hdr->icmp6_type = ICMP6_ECHO_REQUEST;
    icmp6_hdr->icmp6_code = 0;
    icmp6_hdr->icmp6_id = getpid() & 0xFFFF;
    icmp6_hdr->icmp6_seq = 1;
    
    /* 填充数据 */
    for (int i = 0; i < packet_size; i++) {
        packet[sizeof(struct icmp6_hdr) + i] = i & 0xFF;
    }
    
    /* IPv6 内核自动计算校验和 */
    
    /* 设置超时 */
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(pinger->sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    /* 发送 ICMPv6 Echo Request */
    struct timeval send_time;
    gettimeofday(&send_time, nullptr);
    
    ssize_t sent = sendto(pinger->sockfd, packet, packet_len, 0,
                          (struct sockaddr*)dest_addr, sizeof(*dest_addr));
    if (sent < 0) {
        snprintf(result.error_msg, sizeof(result.error_msg), 
                "Failed to send packet: %s", strerror(errno));
        free(packet);
        return result;
    }
    
    /* 接收 ICMPv6 Echo Reply */
    uint8_t recv_buf[4096];
    struct sockaddr_in6 recv_addr;
    socklen_t recv_addr_len = sizeof(recv_addr);
    
    ssize_t received = recvfrom(pinger->sockfd, recv_buf, sizeof(recv_buf), 0,
                               (struct sockaddr*)&recv_addr, &recv_addr_len);
    
    struct timeval recv_time;
    gettimeofday(&recv_time, nullptr);
    
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            snprintf(result.error_msg, sizeof(result.error_msg), "Timeout");
        } else {
            snprintf(result.error_msg, sizeof(result.error_msg), 
                    "Failed to receive: %s", strerror(errno));
        }
        free(packet);
        return result;
    }
    
    /* 解析 ICMPv6 头 */
    struct icmp6_hdr* recv_icmp6 = (struct icmp6_hdr*)recv_buf;
    
    /* 验证 ICMPv6 Echo Reply */
    if (recv_icmp6->icmp6_type == ICMP6_ECHO_REPLY &&
        recv_icmp6->icmp6_id == (getpid() & 0xFFFF)) {
        
        /* 计算延迟 */
        double latency = (recv_time.tv_sec - send_time.tv_sec) * 1000.0 +
                        (recv_time.tv_usec - send_time.tv_usec) / 1000.0;
        
        result.success = true;
        result.latency_ms = latency;
    } else {
        snprintf(result.error_msg, sizeof(result.error_msg), 
                "Invalid ICMPv6 reply (type=%d)", recv_icmp6->icmp6_type);
    }
    
    free(packet);
    return result;
}

ping_result_t icmp_pinger_ping(icmp_pinger_t* restrict pinger, const char* restrict target,
                               int timeout_ms, int packet_size) {
    ping_result_t result = {false, 0.0, ""};
    
    if (pinger == nullptr || target == nullptr) {
        snprintf(result.error_msg, sizeof(result.error_msg), "Invalid arguments");
        return result;
    }
    
    /* 解析目标地址 */
    struct sockaddr_storage addr;
    socklen_t addr_len;
    
    if (!resolve_target(target, pinger->use_ipv6, &addr, &addr_len,
                       result.error_msg, sizeof(result.error_msg))) {
        return result;
    }
    
    /* 执行 ping */
    if (pinger->use_ipv6) {
        return ping_ipv6(pinger, (struct sockaddr_in6*)&addr, timeout_ms, packet_size);
    } else {
        return ping_ipv4(pinger, (struct sockaddr_in*)&addr, timeout_ms, packet_size);
    }
}
