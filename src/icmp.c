#include "openups.h"
#include <linux/filter.h>

uint16_t calculate_checksum(const void *data, size_t len) {
  const uint16_t *buf = (const uint16_t *)data;
  uint32_t sum = 0;

  for (size_t i = 0; i < len / 2; i++) {
    sum += buf[i];
  }
  if (len % 2) {
    sum += ((const uint8_t *)data)[len - 1];
  }
  while (sum >> 16) {
    sum = (sum & 0xFFFF) + (sum >> 16);
  }
  return (uint16_t)(~sum);
}

bool icmp_pinger_init(icmp_pinger_t *restrict pinger, int family,
                      char *restrict error_msg, size_t error_size) {
  if (pinger == NULL || error_msg == NULL || error_size == 0) {
    return false;
  }

  pinger->sockfd = -1;
  pinger->family = family;
  pinger->sequence = 0;
  pinger->send_buf_capacity = sizeof(pinger->send_buf);
  pinger->payload_filled_size = 0;

  int proto = (family == AF_INET6) ? IPPROTO_ICMPV6 : IPPROTO_ICMP;

  /* Create raw socket, CLOEXEC for security, NONBLOCK for poll multiplexing */
  pinger->sockfd =
      socket(family, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, proto);
  if (pinger->sockfd < 0) {
    snprintf(
        error_msg, error_size,
        "Failed to create socket (family=%d): %s (require root or CAP_NET_RAW)",
        family, strerror(errno));
    return false;
  }

  /* Attach kernel BPF filter to drop irrelevant ICMP packets (Phase 2
   * optimization) */
  if (family == AF_INET) {
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_B | BPF_ABS,
                 0), /* A = IP Header Length (ip[0]) */
        BPF_STMT(BPF_ALU | BPF_AND | BPF_K, 0x0f), /* A = A & 0x0f */
        BPF_STMT(BPF_ALU | BPF_MUL | BPF_K, 4),    /* A = A * 4    */
        BPF_STMT(BPF_MISC | BPF_TAX, 0), /* X = A                        */
        BPF_STMT(BPF_LD | BPF_B | BPF_IND, 0), /* A = ip[X] (ICMP Type) */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ICMP_ECHOREPLY, 0,
                 1),                       /* if A == ECHOREPLY, pass  */
        BPF_STMT(BPF_RET | BPF_K, 0xffff), /* Accept (keep full packet)    */
        BPF_STMT(BPF_RET | BPF_K, 0)       /* Reject (drop packet)         */
    };
    struct sock_fprog fprog = {
        .len = (unsigned short)(sizeof(filter) / sizeof(filter[0])),
        .filter = filter};
    setsockopt(pinger->sockfd, SOL_SOCKET, SO_ATTACH_FILTER, &fprog,
               sizeof(fprog));
  } else if (family == AF_INET6) {
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_B | BPF_ABS, 0), /* A = icmp6[0] (ICMPv6 Type) */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ICMP6_ECHO_REPLY, 0,
                 1),                       /* if A == ECHO_REPLY, pass*/
        BPF_STMT(BPF_RET | BPF_K, 0xffff), /* Accept (keep full packet)    */
        BPF_STMT(BPF_RET | BPF_K, 0)       /* Reject (drop packet)         */
    };
    struct sock_fprog fprog = {
        .len = (unsigned short)(sizeof(filter) / sizeof(filter[0])),
        .filter = filter};
    setsockopt(pinger->sockfd, SOL_SOCKET, SO_ATTACH_FILTER, &fprog,
               sizeof(fprog));
  }

  return true;
}

void icmp_pinger_destroy(icmp_pinger_t *restrict pinger) {
  if (pinger == NULL) {
    return;
  }

  if (pinger->sockfd >= 0) {
    close(pinger->sockfd);
    pinger->sockfd = -1;
  }

  pinger->send_buf_capacity = sizeof(pinger->send_buf);
  pinger->payload_filled_size = 0;
}

bool resolve_target(const char *restrict target,
                    struct sockaddr_storage *restrict addr,
                    socklen_t *restrict addr_len, char *restrict error_msg,
                    size_t error_size) {
  if (target == NULL || addr == NULL || addr_len == NULL || error_msg == NULL ||
      error_size == 0) {
    return false;
  }

  memset(addr, 0, sizeof(*addr));

  struct sockaddr_in *addr4 = (struct sockaddr_in *)addr;
  if (inet_pton(AF_INET, target, &addr4->sin_addr) == 1) {
    addr4->sin_family = AF_INET;
    *addr_len = sizeof(*addr4);
    return true;
  }

  struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)addr;
  if (inet_pton(AF_INET6, target, &addr6->sin6_addr) == 1) {
    addr6->sin6_family = AF_INET6;
    *addr_len = sizeof(*addr6);
    return true;
  }

  snprintf(error_msg, error_size,
           "Invalid IPv4/IPv6 address (DNS disabled): %s", target);
  return false;
}

void fill_payload_pattern(icmp_pinger_t *restrict pinger, size_t header_size,
                          size_t payload_size) {
  if (pinger == NULL) {
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

uint16_t next_sequence(icmp_pinger_t *restrict pinger) {
  if (pinger == NULL) {
    return 1;
  }

  /* Skip sequence number 0 */
  if (pinger->sequence == 0xFFFF) {
    pinger->sequence = 1;
  } else {
    pinger->sequence = (uint16_t)(pinger->sequence + 1);
  }
  return pinger->sequence;
}
