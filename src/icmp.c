/* 必须在包含任何头文件之前定义 BSD 宏 */
#define __USE_BSD
#define __FAVOR_BSD

#include "openups_internal.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/icmp6.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* IPv4 ICMP checksum 可选 AVX2 加速（x86 + GCC/Clang）；运行时按 CPU 特性分发。
 * 语义保持与标量版本一致（含 odd-byte 处理）。
 */
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
#include <immintrin.h>
#define OPENUPS_CAN_USE_CPU_SUPPORTS 1
#else
#define OPENUPS_CAN_USE_CPU_SUPPORTS 0
#endif

/* 编译时检查 ICMP 结构体大小 */
static_assert(sizeof(struct icmphdr) >= 8, "icmphdr must be at least 8 bytes");
static_assert(sizeof(struct icmp6_hdr) >= 8, "icmp6_hdr must be at least 8 bytes");

/* ICMP 校验和（IPv4 需要，IPv6 由内核处理）。 */
static uint16_t calculate_checksum_scalar(const void* data, size_t len)
{
    const uint16_t* buf = (const uint16_t*)data;
    uint32_t sum = 0;

    /* 累加 16-bit words */
    for (size_t i = 0; i < len / 2; i++) {
        sum += buf[i];
    }

    /* 奇数字节：保持既有语义（按原字节值直接累加）。 */
    if (len % 2) {
        sum += ((const uint8_t*)data)[len - 1];
    }

    /* 处理进位 */
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return ~sum;
}

#if OPENUPS_CAN_USE_CPU_SUPPORTS
__attribute__((target("avx2")))
static uint16_t calculate_checksum_avx2(const void* data, size_t len)
{
    const uint8_t* bytes = (const uint8_t*)data;
    uint32_t sum = 0;

    /* Process 32-byte blocks (16 x uint16_t). */
    size_t i = 0;

    const __m256i mask00ff = _mm256_set1_epi16(0x00FF);
    const __m256i ones = _mm256_set1_epi16(1);
    __m256i acc32 = _mm256_setzero_si256();

    size_t vec_len = len & ~(size_t)31;
    for (; i < vec_len; i += 32) {
        __m256i v = _mm256_loadu_si256((const __m256i*)(const void*)(bytes + i));

        /* Interpret as 16-bit little-endian words:
         * word = even_byte + (odd_byte << 8)
         */
        __m256i even16 = _mm256_and_si256(v, mask00ff);
        __m256i odd16 = _mm256_and_si256(_mm256_srli_epi16(v, 8), mask00ff);

        /* Pairwise sum into 32-bit lanes. */
        __m256i even32 = _mm256_madd_epi16(even16, ones);
        __m256i odd32 = _mm256_madd_epi16(odd16, ones);
        __m256i words32 = _mm256_add_epi32(even32, _mm256_slli_epi32(odd32, 8));

        acc32 = _mm256_add_epi32(acc32, words32);
    }

    /* Horizontal sum of 8 x u32 lanes. */
    __m128i lo = _mm256_castsi256_si128(acc32);
    __m128i hi = _mm256_extracti128_si256(acc32, 1);
    __m128i s = _mm_add_epi32(lo, hi);
    s = _mm_hadd_epi32(s, s);
    s = _mm_hadd_epi32(s, s);
    sum += (uint32_t)_mm_cvtsi128_si32(s);

    /* Tail: process remaining 16-bit words scalar. */
    size_t tail_start = vec_len;
    const uint16_t* tail16 = (const uint16_t*)(const void*)(bytes + tail_start);
    size_t tail_words = (len - tail_start) / 2;
    for (size_t w = 0; w < tail_words; w++) {
        sum += tail16[w];
    }

    /* Odd byte: preserve existing semantics (added as-is, not shifted). */
    if (len % 2) {
        sum += bytes[len - 1];
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFFU) + (sum >> 16);
    }

    return (uint16_t)(~sum);
}
#endif

static uint16_t calculate_checksum(const void* data, size_t len)
{
#if OPENUPS_CAN_USE_CPU_SUPPORTS
    /* GCC/Clang on x86: safe runtime dispatch. */
    if (__builtin_cpu_supports("avx2")) {
        return calculate_checksum_avx2(data, len);
    }
#endif
    return calculate_checksum_scalar(data, len);
}

/* 初始化 ICMP pinger（需要 CAP_NET_RAW 权限） */
bool icmp_pinger_init(icmp_pinger_t* restrict pinger, bool use_ipv6, char* restrict error_msg,
                      size_t error_size)
{
    if (pinger == NULL || error_msg == NULL || error_size == 0) {
        return false;
    }

    pinger->use_ipv6 = use_ipv6;
    pinger->sockfd = -1;
    pinger->sequence = 0;
    pinger->send_buf = NULL;
    pinger->send_buf_capacity = 0;
    pinger->payload_filled_size = 0;
    pinger->cached_target_valid = false;
    pinger->cached_target[0] = '\0';
    memset(&pinger->cached_addr, 0, sizeof(pinger->cached_addr));
    pinger->cached_addr_len = 0;

    /* 创建 raw socket */
    int family = use_ipv6 ? AF_INET6 : AF_INET;
    int protocol = use_ipv6 ? IPPROTO_ICMPV6 : IPPROTO_ICMP;

    int sock_type = SOCK_RAW;
#ifdef SOCK_CLOEXEC
    sock_type |= SOCK_CLOEXEC;
#endif
    pinger->sockfd = socket(family, sock_type, protocol);
    if (pinger->sockfd < 0) {
        snprintf(error_msg, error_size, "Failed to create socket: %s (require root or CAP_NET_RAW)",
                 strerror(errno));
        return false;
    }

    /* IPv6: 让内核自动计算/验证 ICMPv6 校验和。 */
    if (use_ipv6) {
        int offset = (int)offsetof(struct icmp6_hdr, icmp6_cksum);
        if (setsockopt(pinger->sockfd, IPPROTO_IPV6, IPV6_CHECKSUM, &offset,
                       (socklen_t)sizeof(offset)) != 0) {
            snprintf(error_msg, error_size, "Failed to set IPV6_CHECKSUM: %s", strerror(errno));
            close(pinger->sockfd);
            pinger->sockfd = -1;
            return false;
        }
    }

    /* 非阻塞：避免极端情况下 recvfrom 意外阻塞，配合 poll + EAGAIN 逻辑 */
    int flags = fcntl(pinger->sockfd, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(pinger->sockfd, F_SETFL, flags | O_NONBLOCK);
    }

    return true;
}

/* 销毁 ICMP pinger 并关闭 raw socket */
void icmp_pinger_destroy(icmp_pinger_t* restrict pinger)
{
    if (pinger == NULL) {
        return;
    }

    if (pinger->sockfd >= 0) {
        close(pinger->sockfd);
        pinger->sockfd = -1;
    }

    free(pinger->send_buf);
    pinger->send_buf = NULL;
    pinger->send_buf_capacity = 0;
    pinger->payload_filled_size = 0;

    pinger->cached_target_valid = false;
    pinger->cached_target[0] = '\0';
    memset(&pinger->cached_addr, 0, sizeof(pinger->cached_addr));
    pinger->cached_addr_len = 0;
}

/* 解析目标地址 (IPv4 或 IPv6)
 * 约定: 仅支持 IP 字面量，不做 DNS 解析
 */
static bool resolve_target(const char* restrict target, bool use_ipv6,
                           struct sockaddr_storage* restrict addr, socklen_t* restrict addr_len,
                           char* restrict error_msg, size_t error_size)
{
    if (target == NULL || addr == NULL || addr_len == NULL || error_msg == NULL ||
        error_size == 0) {
        return false;
    }

    memset(addr, 0, sizeof(*addr));

    if (use_ipv6) {
        struct sockaddr_in6* addr6 = (struct sockaddr_in6*)addr;
        addr6->sin6_family = AF_INET6;
        if (inet_pton(AF_INET6, target, &addr6->sin6_addr) != 1) {
            snprintf(error_msg, error_size, "Invalid IPv6 address (DNS disabled): %s", target);
            return false;
        }
        *addr_len = sizeof(*addr6);
        return true;
    }

    struct sockaddr_in* addr4 = (struct sockaddr_in*)addr;
    addr4->sin_family = AF_INET;
    if (inet_pton(AF_INET, target, &addr4->sin_addr) != 1) {
        snprintf(error_msg, error_size, "Invalid IPv4 address (DNS disabled): %s", target);
        return false;
    }
    *addr_len = sizeof(*addr4);
    return true;
}

static bool ensure_send_buffer(icmp_pinger_t* restrict pinger, size_t need,
                               char* restrict error_msg, size_t error_size)
{
    if (pinger == NULL || error_msg == NULL || error_size == 0) {
        return false;
    }

    if (need <= pinger->send_buf_capacity && pinger->send_buf != NULL) {
        return true;
    }

    uint8_t* old_buf = pinger->send_buf;
    uint8_t* new_buf = (uint8_t*)realloc(pinger->send_buf, need);
    if (new_buf == NULL) {
        snprintf(error_msg, error_size, "Memory allocation failed");
        return false;
    }

    pinger->send_buf = new_buf;
    pinger->send_buf_capacity = need;
    if (new_buf != old_buf) {
        pinger->payload_filled_size = 0;
    }
    return true;
}

static void fill_payload_pattern(icmp_pinger_t* restrict pinger, size_t header_size,
                                 size_t payload_size)
{
    if (pinger == NULL || pinger->send_buf == NULL) {
        return;
    }

    if (payload_size == 0) {
        pinger->payload_filled_size = 0;
        return;
    }

    if (pinger->payload_filled_size == payload_size) {
        return;
    }

    for (size_t i = 0; i < payload_size; i++) {
        pinger->send_buf[header_size + i] = (uint8_t)(i & 0xFFU);
    }

    pinger->payload_filled_size = payload_size;
}

static uint16_t next_sequence(icmp_pinger_t* restrict pinger)
{
    if (pinger == NULL) {
        return 1;
    }

    /* 0 作为序列号可用但不常见；这里避免长期停留在 0。 */
    pinger->sequence = (uint16_t)(pinger->sequence + 1);
    if (pinger->sequence == 0) {
        pinger->sequence = 1;
    }
    return pinger->sequence;
}

static int wait_readable_with_tick(int fd, uint64_t deadline_ms, icmp_tick_fn tick,
                                   void* tick_user_data, icmp_should_stop_fn should_stop,
                                   void* stop_user_data)
{
    uint64_t last_tick_ms = 0;

    while (1) {
        if (should_stop != NULL && should_stop(stop_user_data)) {
            errno = EINTR;
            return -2;
        }

        uint64_t now_ms = get_monotonic_ms();
        if (now_ms == UINT64_MAX) {
            errno = EINVAL;
            return -1;
        }

        if (now_ms >= deadline_ms) {
            errno = EAGAIN;
            return -1;
        }

        /* 默认每 1 秒 tick 一次，避免过于频繁 */
        if (tick != NULL && (last_tick_ms == 0 || now_ms - last_tick_ms >= 1000)) {
            tick(tick_user_data);
            last_tick_ms = now_ms;
        }

        int timeout_ms = (int)(deadline_ms - now_ms);
        if (timeout_ms > 200) {
            timeout_ms = 200;
        }
        if (timeout_ms < 1) {
            timeout_ms = 1;
        }

        struct pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};

        int rc = poll(&pfd, 1, timeout_ms);
        if (rc > 0) {
            if ((pfd.revents & POLLIN) != 0) {
                return 0;
            }
            /* 其它事件（如错误）也让上层去 recvfrom 触发 errno */
            return 0;
        }
        if (rc == 0) {
            continue;
        }

        if (errno == EINTR) {
            continue;
        }
        return -1;
    }
}

/* IPv4 Ping 实现 (RFC 792)
 * 流程: 构造 ICMP Echo Request → 发送 → 等待 Echo Reply → 计算延迟
 * 特殊注意: 需要手动计算校验和, 需要 CAP_NET_RAW 权限
 */
static ping_result_t ping_ipv4(icmp_pinger_t* restrict pinger,
                               struct sockaddr_in* restrict dest_addr, int timeout_ms,
                               int payload_size, icmp_tick_fn tick, void* tick_user_data,
                               icmp_should_stop_fn should_stop, void* stop_user_data)
{
    ping_result_t result = {false, 0.0, ""};

    if (pinger == NULL || dest_addr == NULL) {
        snprintf(result.error_msg, sizeof(result.error_msg), "Invalid arguments");
        return result;
    }

    if (timeout_ms <= 0) {
        snprintf(result.error_msg, sizeof(result.error_msg), "Invalid timeout");
        return result;
    }

    /* 构建 ICMP Echo Request 数据包（复用缓冲区） */
    size_t packet_len = sizeof(struct icmphdr) + (size_t)payload_size;
    if (!ensure_send_buffer(pinger, packet_len, result.error_msg, sizeof(result.error_msg))) {
        return result;
    }
    fill_payload_pattern(pinger, sizeof(struct icmphdr), (size_t)payload_size);

    struct icmphdr* icmp_hdr = (struct icmphdr*)pinger->send_buf;
    memset(icmp_hdr, 0, sizeof(*icmp_hdr));
    icmp_hdr->type = ICMP_ECHO;
    icmp_hdr->code = 0;
    uint16_t ident = (uint16_t)(getpid() & 0xFFFF);
    uint16_t seq = next_sequence(pinger);
    icmp_hdr->un.echo.id = ident;
    icmp_hdr->un.echo.sequence = seq;

    /* 计算校验和 (标准流程: 先清零, 再计算) */
    icmp_hdr->checksum = 0;
    icmp_hdr->checksum = calculate_checksum(pinger->send_buf, packet_len);

    /* 发送 ICMP Echo Request */
    struct timespec send_time;
    (void)clock_gettime(CLOCK_MONOTONIC, &send_time);

    ssize_t sent = sendto(pinger->sockfd, pinger->send_buf, packet_len, 0,
                          (struct sockaddr*)dest_addr,
                          sizeof(*dest_addr));
    if (sent < 0) {
        snprintf(result.error_msg, sizeof(result.error_msg), "Failed to send packet: %s",
                 strerror(errno));
        return result;
    }

    /* 接收 ICMP Echo Reply (包含 IP 头和 ICMP 头) */
    uint8_t recv_buf[4096];
    struct sockaddr_in recv_addr;
    socklen_t recv_addr_len = sizeof(recv_addr);

    uint64_t send_ms = (uint64_t)send_time.tv_sec * 1000ULL +
                       (uint64_t)send_time.tv_nsec / 1000000ULL;
    uint64_t deadline_ms = send_ms + (uint64_t)timeout_ms;

    while (1) {
        int wait_rc = wait_readable_with_tick(pinger->sockfd, deadline_ms, tick, tick_user_data,
                                              should_stop, stop_user_data);
        if (wait_rc == -2) {
            snprintf(result.error_msg, sizeof(result.error_msg), "Stopped");
            return result;
        }
        if (wait_rc < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                snprintf(result.error_msg, sizeof(result.error_msg), "Timeout");
            } else {
                snprintf(result.error_msg, sizeof(result.error_msg), "Failed to wait: %s",
                         strerror(errno));
            }
            return result;
        }

        ssize_t received;
        recv_addr_len = sizeof(recv_addr);
        do {
            received = recvfrom(pinger->sockfd, recv_buf, sizeof(recv_buf), 0,
                                (struct sockaddr*)&recv_addr, &recv_addr_len);
        } while (received < 0 && errno == EINTR);

        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            snprintf(result.error_msg, sizeof(result.error_msg), "Failed to receive: %s",
                     strerror(errno));
            return result;
        }

        /* 快速过滤：只接受来自目标的回包 */
        if (recv_addr.sin_addr.s_addr != dest_addr->sin_addr.s_addr) {
            continue;
        }

        /* 解析 IP 头和 ICMP 头 (IP 头长度可变) */
        if (received < (ssize_t)sizeof(struct ip)) {
            continue;
        }

        struct ip* ip_hdr = (struct ip*)recv_buf;
        if (ip_hdr->ip_p != IPPROTO_ICMP) {
            continue;
        }

        size_t ip_hdr_len = (size_t)ip_hdr->ip_hl * 4;
        if (ip_hdr_len < sizeof(struct ip) || ip_hdr_len > (size_t)received) {
            continue;
        }

        if (ip_hdr_len + sizeof(struct icmphdr) > (size_t)received) {
            continue;
        }

        struct icmphdr* recv_icmp = (struct icmphdr*)(recv_buf + ip_hdr_len);
        if (recv_icmp->type != ICMP_ECHOREPLY) {
            continue;
        }
        if (recv_icmp->un.echo.id != ident || recv_icmp->un.echo.sequence != seq) {
            continue;
        }

        struct timespec recv_time;
        (void)clock_gettime(CLOCK_MONOTONIC, &recv_time);

        double latency = (recv_time.tv_sec - send_time.tv_sec) * 1000.0 +
                         (recv_time.tv_nsec - send_time.tv_nsec) / 1000000.0;
        if (latency < 0.0) {
            latency = 0.0;
        }

        result.success = true;
        result.latency_ms = latency;
        return result;
    }
}

/* IPv6 Ping 实现 (RFC 4443)
 * 流程: 构造 ICMPv6 Echo Request → 发送 → 等待 Echo Reply → 计算延迟
 * 特殊注意:
 *   1. 校验和由内核自动计算 (IPV6_CHECKSUM socket 选项)
 *   2. 使用 ICMPv6 报头和类型
 */
static ping_result_t ping_ipv6(icmp_pinger_t* restrict pinger,
                               struct sockaddr_in6* restrict dest_addr, int timeout_ms,
                               int payload_size, icmp_tick_fn tick, void* tick_user_data,
                               icmp_should_stop_fn should_stop, void* stop_user_data)
{
    ping_result_t result = {false, 0.0, ""};

    if (pinger == NULL || dest_addr == NULL) {
        snprintf(result.error_msg, sizeof(result.error_msg), "Invalid arguments");
        return result;
    }

    if (timeout_ms <= 0) {
        snprintf(result.error_msg, sizeof(result.error_msg), "Invalid timeout");
        return result;
    }

    /* 构造 ICMPv6 Echo Request 数据包（复用缓冲区） */
    size_t packet_len = sizeof(struct icmp6_hdr) + (size_t)payload_size;
    if (!ensure_send_buffer(pinger, packet_len, result.error_msg, sizeof(result.error_msg))) {
        return result;
    }
    fill_payload_pattern(pinger, sizeof(struct icmp6_hdr), (size_t)payload_size);

    struct icmp6_hdr* icmp6_hdr = (struct icmp6_hdr*)pinger->send_buf;
    memset(icmp6_hdr, 0, sizeof(*icmp6_hdr));
    icmp6_hdr->icmp6_type = ICMP6_ECHO_REQUEST; /* 类型: Echo Request = 128 */
    icmp6_hdr->icmp6_code = 0;                  /* 代码: 0 */
    uint16_t ident = (uint16_t)(getpid() & 0xFFFF);
    uint16_t seq = next_sequence(pinger);
    icmp6_hdr->icmp6_id = ident;
    icmp6_hdr->icmp6_seq = seq;

    /* IPv6 内核自动计算校验和 (不需手动设置) */

    /* 发送 ICMPv6 Echo Request */
    struct timespec send_time;
    (void)clock_gettime(CLOCK_MONOTONIC, &send_time);

    ssize_t sent = sendto(pinger->sockfd, pinger->send_buf, packet_len, 0,
                          (struct sockaddr*)dest_addr,
                          sizeof(*dest_addr));
    if (sent < 0) {
        snprintf(result.error_msg, sizeof(result.error_msg), "Failed to send packet: %s",
                 strerror(errno));
        return result;
    }

    /* 接收 ICMPv6 Echo Reply */
    uint8_t recv_buf[4096];
    struct sockaddr_in6 recv_addr;
    socklen_t recv_addr_len = sizeof(recv_addr);

    uint64_t send_ms = (uint64_t)send_time.tv_sec * 1000ULL +
                       (uint64_t)send_time.tv_nsec / 1000000ULL;
    uint64_t deadline_ms = send_ms + (uint64_t)timeout_ms;

    while (1) {
        int wait_rc = wait_readable_with_tick(pinger->sockfd, deadline_ms, tick, tick_user_data,
                                              should_stop, stop_user_data);
        if (wait_rc == -2) {
            snprintf(result.error_msg, sizeof(result.error_msg), "Stopped");
            return result;
        }
        if (wait_rc < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                snprintf(result.error_msg, sizeof(result.error_msg), "Timeout");
            } else {
                snprintf(result.error_msg, sizeof(result.error_msg), "Failed to wait: %s",
                         strerror(errno));
            }
            return result;
        }

        ssize_t received;
        recv_addr_len = sizeof(recv_addr);
        do {
            received = recvfrom(pinger->sockfd, recv_buf, sizeof(recv_buf), 0,
                                (struct sockaddr*)&recv_addr, &recv_addr_len);
        } while (received < 0 && errno == EINTR);

        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            snprintf(result.error_msg, sizeof(result.error_msg), "Failed to receive: %s",
                     strerror(errno));
            return result;
        }

        /* 过滤源地址 */
        if (memcmp(&recv_addr.sin6_addr, &dest_addr->sin6_addr, sizeof(struct in6_addr)) != 0) {
            continue;
        }

        if (received < (ssize_t)sizeof(struct icmp6_hdr)) {
            continue;
        }

        struct icmp6_hdr* recv_icmp6 = (struct icmp6_hdr*)recv_buf;
        if (recv_icmp6->icmp6_type != ICMP6_ECHO_REPLY) {
            continue;
        }
        if (recv_icmp6->icmp6_id != ident || recv_icmp6->icmp6_seq != seq) {
            continue;
        }

        struct timespec recv_time;
        (void)clock_gettime(CLOCK_MONOTONIC, &recv_time);

        double latency = (recv_time.tv_sec - send_time.tv_sec) * 1000.0 +
                         (recv_time.tv_nsec - send_time.tv_nsec) / 1000000.0;
        if (latency < 0.0) {
            latency = 0.0;
        }

        result.success = true;
        result.latency_ms = latency;
        return result;
    }
}

/* 执行 ping 操作 (自动根据地址类型选择 IPv4/IPv6） */
ping_result_t icmp_pinger_ping(icmp_pinger_t* restrict pinger, const char* restrict target,
                               int timeout_ms, int payload_size)
{
    return icmp_pinger_ping_ex(pinger, target, timeout_ms, payload_size, NULL, NULL, NULL, NULL);
}

ping_result_t icmp_pinger_ping_ex(icmp_pinger_t* restrict pinger, const char* restrict target,
                                  int timeout_ms, int payload_size, icmp_tick_fn tick,
                                  void* tick_user_data, icmp_should_stop_fn should_stop,
                                  void* stop_user_data)
{
    ping_result_t result = {false, 0.0, ""};

    if (pinger == NULL || target == NULL) {
        snprintf(result.error_msg, sizeof(result.error_msg), "Invalid arguments");
        return result;
    }

    /* 解析目标地址（带缓存） */
    const struct sockaddr_storage* addr_ptr = NULL;
    socklen_t addr_len = 0;

    if (pinger->cached_target_valid &&
        strncmp(pinger->cached_target, target, sizeof(pinger->cached_target)) == 0) {
        addr_ptr = &pinger->cached_addr;
        addr_len = pinger->cached_addr_len;
    } else {
        struct sockaddr_storage addr;
        if (!resolve_target(target, pinger->use_ipv6, &addr, &addr_len, result.error_msg,
                            sizeof(result.error_msg))) {
            return result;
        }

        /* 尽量缓存（target 来自 config/CLI，正常长度 < 256） */
        (void)snprintf(pinger->cached_target, sizeof(pinger->cached_target), "%s", target);
        pinger->cached_addr = addr;
        pinger->cached_addr_len = addr_len;
        pinger->cached_target_valid = true;
        addr_ptr = &pinger->cached_addr;
    }

    (void)addr_len;
    if (pinger->use_ipv6) {
        return ping_ipv6(pinger, (struct sockaddr_in6*)(void*)addr_ptr, timeout_ms, payload_size,
                         tick, tick_user_data, should_stop, stop_user_data);
    }
    return ping_ipv4(pinger, (struct sockaddr_in*)(void*)addr_ptr, timeout_ms, payload_size, tick,
                     tick_user_data, should_stop, stop_user_data);
}
