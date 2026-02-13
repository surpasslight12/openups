#include "systemd.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* ============================================================
 * systemd 通知器
 * ============================================================ */

static bool build_systemd_addr(const char* restrict socket_path, struct sockaddr_un* restrict addr,
                              socklen_t* restrict addr_len)
{
    if (socket_path == NULL || addr == NULL || addr_len == NULL) {
        return false;
    }

    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;

    /* 处理抽象命名空间（@ 前缀） */
    if (socket_path[0] == '@') {
        size_t name_len = strlen(socket_path + 1);
        if (name_len == 0 || name_len > sizeof(addr->sun_path) - 1) {
            return false;
        }

        addr->sun_path[0] = '\0';
        memcpy(addr->sun_path + 1, socket_path + 1, name_len);
        *addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + name_len);
        return true;
    }

    size_t path_len = strlen(socket_path);
    if (path_len == 0 || path_len >= sizeof(addr->sun_path)) {
        return false;
    }

    memcpy(addr->sun_path, socket_path, path_len + 1);
    *addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_len + 1);
    return true;
}

/* 发送 systemd 通知消息（READY/STATUS/STOPPING/WATCHDOG）。 */
static OPENUPS_HOT bool send_notify(systemd_notifier_t* restrict notifier, const char* restrict message)
{
    if (OPENUPS_UNLIKELY(notifier == NULL || message == NULL || !notifier->enabled)) {
        return false;
    }

    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif

    /* 重试发送直到成功或遇到非 EINTR 错误 */
    ssize_t sent;
    do {
        sent = send(notifier->sockfd, message, strlen(message), flags);
    } while (sent < 0 && errno == EINTR);

    return sent >= 0;
}

void systemd_notifier_init(systemd_notifier_t* restrict notifier)
{
    if (notifier == NULL) {
        return;
    }

    notifier->enabled = false;
    notifier->sockfd = -1;
    notifier->watchdog_usec = 0;
    memset(&notifier->addr, 0, sizeof(notifier->addr));
    notifier->addr_len = 0;
    notifier->last_watchdog_ms = 0;
    notifier->last_status_ms = 0;
    notifier->last_status[0] = '\0';

    /* 非 systemd 管理时通常不会设置 NOTIFY_SOCKET。 */
    const char* socket_path = getenv("NOTIFY_SOCKET");
    if (socket_path == NULL) {
        return;
    }

    /* systemd 通知使用 AF_UNIX/SOCK_DGRAM。 */
    int socket_type = SOCK_DGRAM;
#ifdef SOCK_CLOEXEC
    socket_type |= SOCK_CLOEXEC;
#endif
    notifier->sockfd = socket(AF_UNIX, socket_type, 0);
    if (notifier->sockfd < 0) {
        return;
    }

    if (!build_systemd_addr(socket_path, &notifier->addr, &notifier->addr_len)) {
        close(notifier->sockfd);
        notifier->sockfd = -1;
        return;
    }

    int rc;
    do {
        rc = connect(notifier->sockfd, (const struct sockaddr*)&notifier->addr, notifier->addr_len);
    } while (rc < 0 && errno == EINTR);
    if (rc < 0) {
        close(notifier->sockfd);
        notifier->sockfd = -1;
        return;
    }

    notifier->enabled = true;

    /* watchdog 超时时间（微秒）；用于计算心跳间隔。 */
    const char* watchdog_str = getenv("WATCHDOG_USEC");
    if (watchdog_str != NULL) {
        notifier->watchdog_usec = strtoull(watchdog_str, NULL, 10);
    }
}

void systemd_notifier_destroy(systemd_notifier_t* restrict notifier)
{
    if (notifier == NULL) {
        return;
    }

    if (notifier->sockfd >= 0) {
        close(notifier->sockfd);
        notifier->sockfd = -1;
    }

    notifier->enabled = false;
    memset(&notifier->addr, 0, sizeof(notifier->addr));
    notifier->addr_len = 0;
}

/* 检查 systemd 通知器是否已启用 */
bool systemd_notifier_is_enabled(const systemd_notifier_t* restrict notifier)
{
    return notifier != NULL && notifier->enabled;
}

/* 通知 systemd 程序已就绪 (启动完成) */
bool systemd_notifier_ready(systemd_notifier_t* restrict notifier)
{
    return send_notify(notifier, "READY=1");
}

/* 更新程序状态信息到 systemd */
bool systemd_notifier_status(systemd_notifier_t* restrict notifier, const char* restrict status)
{
    if (notifier == NULL || !notifier->enabled || status == NULL) {
        return false;
    }

    /* 降频：内容未变化且距离上次发送太近时跳过 */
    uint64_t now_ms = get_monotonic_ms();
    bool same = (strncmp(notifier->last_status, status, sizeof(notifier->last_status)) == 0);
    if (same && notifier->last_status_ms != 0 && now_ms - notifier->last_status_ms < 2000) {
        return true;
    }

    /* systemd 约定：STATUS=... */
    char message[256];
    snprintf(message, sizeof(message), "STATUS=%s", status);

    bool ok = send_notify(notifier, message);
    if (ok) {
        snprintf(notifier->last_status, sizeof(notifier->last_status), "%s", status);
        notifier->last_status_ms = now_ms;
    }
    return ok;
}

/* 通知 systemd 程序即将停止运行 */
bool systemd_notifier_stopping(systemd_notifier_t* restrict notifier)
{
    return send_notify(notifier, "STOPPING=1");
}

OPENUPS_HOT bool systemd_notifier_watchdog(systemd_notifier_t* restrict notifier)
{
    if (OPENUPS_UNLIKELY(notifier == NULL || !notifier->enabled)) {
        return false;
    }

    /* 未设置 WatchdogSec 时 systemd 不会提供 WATCHDOG_USEC，此时应默认 no-op。 */
    if (OPENUPS_UNLIKELY(notifier->watchdog_usec == 0)) {
        return true;
    }

    uint64_t now_ms = get_monotonic_ms();
    uint64_t interval_ms = notifier->watchdog_usec / 2000ULL; /* usec/2 -> ms */
    if (interval_ms == 0) {
        interval_ms = 1;
    }

    if (notifier->last_watchdog_ms != 0 && now_ms - notifier->last_watchdog_ms < interval_ms) {
        return true;
    }

    bool ok = send_notify(notifier, "WATCHDOG=1");
    if (ok) {
        notifier->last_watchdog_ms = now_ms;
    }
    return ok;
}

uint64_t systemd_notifier_watchdog_interval_ms(const systemd_notifier_t* restrict notifier)
{
    if (notifier == NULL || !notifier->enabled || notifier->watchdog_usec == 0) {
        return 0;
    }

    uint64_t interval_ms = notifier->watchdog_usec / 2000ULL; /* usec/2 -> ms */
    if (interval_ms == 0) {
        interval_ms = 1;
    }
    return interval_ms;
}
