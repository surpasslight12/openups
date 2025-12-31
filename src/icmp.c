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

/* ICMP 校验和计算 (RFC 792)
 * 算法:
 *   1. 数据分为多个 16 位字，依次累加
 *   2. 如果长度是奇数，最后一个字节作为 16 位字的高位（补零作为低位）
 *   3. 将进位加回到和中
 *   4. 对结果取反码，零为全 1
 * IPv4: 需手动计算，因为 ICMP 是内核上层的协议
 * IPv6: 由内核自动计算 (IPV6_CHECKSUM socket 选项)
 */
static uint16_t calculate_checksum(const void* data, size_t len) {
    const uint16_t* buf = (const uint16_t*)data;
    uint32_t sum = 0;
    
    /* 步骤 1: 累加所有 16 位字 */
    for (size_t i = 0; i < len / 2; i++) {
        sum += buf[i];
    }
    
    /* 步骤 2: 处理奇数位 */
    if (len % 2) {
        sum += ((const uint8_t*)data)[len - 1];
    }
    
    /* 步骤 3: 处理进位 (IP 格式校验和需要一个循环算法) */
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    
    /* 步骤 4: 返回反码 */
    return ~sum;
}

/* 初始化 ICMP pinger（需要 CAP_NET_RAW 权限） */
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
                "Failed to create socket: %s (require root or CAP_NET_RAW)", 
                strerror(errno));
        return false;
    }
    
    return true;
}

/* 销毁 ICMP pinger 并关闭 raw socket */
void icmp_pinger_destroy(icmp_pinger_t* restrict pinger) {
    if (pinger == nullptr) {
        return;
    }
    
    if (pinger->sockfd >= 0) {
        close(pinger->sockfd);
        pinger->sockfd = -1;
    }
}

/* 解析目标地址 (IPv4 或 IPv6)
 * 实现: 使用 getaddrinfo() 解析 DNS 名称或直接 IP 地址
 * 参数: target - 主机名或 IP 地址, use_ipv6 - true 为 IPv6, false 为 IPv4
 */
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

/* IPv4 Ping 实现 (RFC 792)
 * 流程: 构造 ICMP Echo Request → 发送 → 等待 Echo Reply → 计算延迟
 * 特殊注意: 需要手动计算校验和, 需要 CAP_NET_RAW 权限
 */
static ping_result_t ping_ipv4(icmp_pinger_t* restrict pinger, struct sockaddr_in* restrict dest_addr,
                               int timeout_ms, int packet_size) {
    ping_result_t result = {false, 0.0, ""};
    
    if (pinger == nullptr || dest_addr == nullptr) {
        snprintf(result.error_msg, sizeof(result.error_msg), "Invalid arguments");
        return result;
    }
    
    /* 构建 ICMP Echo Request 数据包 */
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
    
    /* 计算校验和 (标准流程: 先清零, 再计算) */
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
    
    /* 接收 ICMP Echo Reply (包含 IP 头和 ICMP 头) */
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
    
    /* 解析 IP 头和 ICMP 头 (IP 头长度可变) */
    struct ip* ip_hdr = (struct ip*)recv_buf;
    size_t ip_hdr_len = ip_hdr->ip_hl * 4;  /* IHL 字段是 32 位单位 */
    struct icmphdr* recv_icmp = (struct icmphdr*)(recv_buf + ip_hdr_len);
    
    /* 验证 ICMP Echo Reply (类型和 ID 必须匹配) */
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

/* IPv6 Ping 实现 (RFC 4443)
 * 流程: 构造 ICMPv6 Echo Request → 发送 → 等待 Echo Reply → 计算延迟
 * 特殊注意:
 *   1. 校验和由内核自动计算 (IPV6_CHECKSUM socket 选项)
 *   2. 使用 ICMPv6 报头和类型
 */
static ping_result_t ping_ipv6(icmp_pinger_t* restrict pinger, struct sockaddr_in6* restrict dest_addr,
                               int timeout_ms, int packet_size) {
    ping_result_t result = {false, 0.0, ""};
    
    if (pinger == nullptr || dest_addr == nullptr) {
        snprintf(result.error_msg, sizeof(result.error_msg), "Invalid arguments");
        return result;
    }
    
    /* 构造 ICMPv6 Echo Request 数据包 (模于 IPv4) */
    size_t packet_len = sizeof(struct icmp6_hdr) + packet_size;
    uint8_t* packet = (uint8_t*)calloc(1, packet_len);
    if (packet == nullptr) {
        snprintf(result.error_msg, sizeof(result.error_msg), "Memory allocation failed");
        return result;
    }
    
    struct icmp6_hdr* icmp6_hdr = (struct icmp6_hdr*)packet;
    icmp6_hdr->icmp6_type = ICMP6_ECHO_REQUEST;  /* 类型: Echo Request = 128 */
    icmp6_hdr->icmp6_code = 0;                   /* 代码: 0 */
    icmp6_hdr->icmp6_id = getpid() & 0xFFFF;    /* 标識符 */
    icmp6_hdr->icmp6_seq = 1;                    /* 序列号 */
    
    /* 填充数据 */
    for (int i = 0; i < packet_size; i++) {
        packet[sizeof(struct icmp6_hdr) + i] = i & 0xFF;
    }
    
    /* IPv6 内核自动计算校验和 (不需手动设置) */
    
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

/* 执行 ping 操作 (自动根据地址类型选择 IPv4/IPv6） */
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
