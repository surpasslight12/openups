#include "openups.h"

#define OPENUPS_NOTIFY_RETRY_COUNT 3
#define OPENUPS_NOTIFY_RETRY_NS 10000000L

static bool parse_uint64_value(const char *restrict value,
                               uint64_t *restrict out_value) {
  if (value == NULL || out_value == NULL) {
    return false;
  }

  errno = 0;
  char *endptr = NULL;
  unsigned long long parsed = strtoull(value, &endptr, 10);
  if (errno != 0 || endptr == value || *endptr != '\0') {
    return false;
  }

  uint64_t converted = (uint64_t)parsed;
  if (converted != parsed) {
    return false;
  }

  *out_value = converted;
  return true;
}

static bool watchdog_pid_matches_current_process(void) {
  const char *watchdog_pid_str = getenv("WATCHDOG_PID");
  if (watchdog_pid_str == NULL || watchdog_pid_str[0] == '\0') {
    return true;
  }

  errno = 0;
  char *endptr = NULL;
  long parsed_pid = strtol(watchdog_pid_str, &endptr, 10);
  if (errno != 0 || endptr == watchdog_pid_str || *endptr != '\0' ||
      parsed_pid <= 0) {
    return false;
  }

  return (pid_t)parsed_pid == getpid();
}

static bool build_systemd_addr(const char *restrict socket_path,
                               struct sockaddr_un *restrict addr,
                               socklen_t *restrict addr_len) {
  if (socket_path == NULL || addr == NULL || addr_len == NULL) {
    return false;
  }

  memset(addr, 0, sizeof(*addr));
  addr->sun_family = AF_UNIX;

  if (socket_path[0] == '@') {
    size_t name_len = strlen(socket_path + 1);
    if (name_len == 0 || name_len > sizeof(addr->sun_path) - 1) {
      return false;
    }

    addr->sun_path[0] = '\0';
    memcpy(addr->sun_path + 1, socket_path + 1, name_len);
    *addr_len =
        (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + name_len);
    return true;
  }

  size_t path_len = strlen(socket_path);
  if (path_len == 0 || path_len >= sizeof(addr->sun_path)) {
    return false;
  }

  memcpy(addr->sun_path, socket_path, path_len + 1);
  *addr_len =
      (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_len + 1);
  return true;
}

static bool send_notify(systemd_notifier_t *restrict notifier,
                        const char *restrict message) {
  if (OPENUPS_UNLIKELY(notifier == NULL || message == NULL ||
                       !notifier->enabled)) {
    return false;
  }

  size_t message_len = strlen(message);
  for (int attempt = 0; attempt < OPENUPS_NOTIFY_RETRY_COUNT; attempt++) {
    ssize_t sent = send(notifier->sockfd, message, message_len, MSG_NOSIGNAL);
    if (sent >= 0) {
      return true;
    }

    if (errno == EINTR) {
      continue;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) {
      struct timespec retry_sleep = {.tv_sec = 0,
                                     .tv_nsec = OPENUPS_NOTIFY_RETRY_NS};
      while (nanosleep(&retry_sleep, &retry_sleep) < 0 && errno == EINTR) {
      }
      continue;
    }

    break;
  }

  return false;
}

void systemd_notifier_init(systemd_notifier_t *restrict notifier) {
  if (notifier == NULL) {
    return;
  }

  notifier->enabled = false;
  notifier->sockfd = -1;
  notifier->watchdog_usec = 0;
  notifier->last_status_ms = 0;
  notifier->last_status[0] = '\0';

  const char *socket_path = getenv("NOTIFY_SOCKET");
  if (socket_path == NULL) {
    return;
  }

  notifier->sockfd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (notifier->sockfd < 0) {
    return;
  }

  struct sockaddr_un addr;
  socklen_t addr_len;
  if (!build_systemd_addr(socket_path, &addr, &addr_len)) {
    close(notifier->sockfd);
    notifier->sockfd = -1;
    return;
  }

  int rc;
  do {
    rc = connect(notifier->sockfd, (const struct sockaddr *)&addr, addr_len);
  } while (rc < 0 && errno == EINTR);
  if (rc < 0) {
    close(notifier->sockfd);
    notifier->sockfd = -1;
    return;
  }

  notifier->enabled = true;

  const char *watchdog_str = getenv("WATCHDOG_USEC");
  if (watchdog_str != NULL && watchdog_pid_matches_current_process()) {
    uint64_t parsed_watchdog_usec = 0;
    if (parse_uint64_value(watchdog_str, &parsed_watchdog_usec)) {
      notifier->watchdog_usec = parsed_watchdog_usec;
    }
  }
}

void systemd_notifier_destroy(systemd_notifier_t *restrict notifier) {
  if (notifier == NULL) {
    return;
  }

  if (notifier->sockfd >= 0) {
    close(notifier->sockfd);
    notifier->sockfd = -1;
  }

  notifier->enabled = false;
}

bool systemd_notifier_is_enabled(
    const systemd_notifier_t *restrict notifier) {
  return notifier != NULL && notifier->enabled;
}

bool systemd_notifier_ready(systemd_notifier_t *restrict notifier) {
  return send_notify(notifier, "READY=1");
}

bool systemd_notifier_status(systemd_notifier_t *restrict notifier,
                             const char *restrict status) {
  if (notifier == NULL || !notifier->enabled || status == NULL) {
    return false;
  }

  uint64_t now_ms = get_monotonic_ms();
  bool same =
      (strncmp(notifier->last_status, status, sizeof(notifier->last_status)) ==
       0);
  if (same && notifier->last_status_ms != 0 && now_ms != UINT64_MAX &&
      now_ms - notifier->last_status_ms < 2000) {
    return true;
  }

  char message[OPENUPS_SYSTEMD_MESSAGE_SIZE];
  snprintf(message, sizeof(message), "STATUS=%.*s",
           (int)OPENUPS_SYSTEMD_STATUS_SIZE - 1, status);
  bool ok = send_notify(notifier, message);
  if (ok) {
    snprintf(notifier->last_status, sizeof(notifier->last_status), "%s",
             status);
    notifier->last_status_ms = now_ms;
  }
  return ok;
}

bool systemd_notifier_stopping(systemd_notifier_t *restrict notifier) {
  return send_notify(notifier, "STOPPING=1");
}

bool systemd_notifier_watchdog(systemd_notifier_t *restrict notifier) {
  if (OPENUPS_UNLIKELY(notifier == NULL || !notifier->enabled ||
                       notifier->watchdog_usec == 0)) {
    return false;
  }
  return send_notify(notifier, "WATCHDOG=1");
}

uint64_t systemd_notifier_watchdog_interval_ms(
    const systemd_notifier_t *restrict notifier) {
  if (notifier == NULL || !notifier->enabled || notifier->watchdog_usec == 0) {
    return 0;
  }

  uint64_t interval_ms = notifier->watchdog_usec / 2000ULL;
  if (interval_ms == 0) {
    interval_ms = 1;
  }
  return interval_ms;
}