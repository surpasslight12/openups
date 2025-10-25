#include "systemd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

/* 发送 systemd 通知 */
static bool send_notify(systemd_notifier_t* notifier, const char* message) {
    if (!notifier->enabled || !notifier->notify_socket) {
        return false;
    }
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    
    /* 处理抽象命名空间（@ 前缀） */
    if (notifier->notify_socket[0] == '@') {
        addr.sun_path[0] = '\0';
        /* 使用 memcpy 而不是 strncpy，因为可能包含 null 字节 */
        size_t len = strlen(notifier->notify_socket + 1);
        if (len >= sizeof(addr.sun_path) - 1) {
            len = sizeof(addr.sun_path) - 2;
        }
        memcpy(addr.sun_path + 1, notifier->notify_socket + 1, len);
    } else {
        size_t len = strlen(notifier->notify_socket);
        if (len >= sizeof(addr.sun_path)) {
            len = sizeof(addr.sun_path) - 1;
        }
        memcpy(addr.sun_path, notifier->notify_socket, len);
        addr.sun_path[len] = '\0';
    }
    
    ssize_t sent = sendto(notifier->sockfd, message, strlen(message), 0,
                         (struct sockaddr*)&addr, sizeof(addr));
    
    return sent >= 0;
}

void systemd_notifier_init(systemd_notifier_t* notifier) {
    notifier->enabled = false;
    notifier->notify_socket = NULL;
    notifier->sockfd = -1;
    notifier->watchdog_usec = 0;
    
    /* 检查 NOTIFY_SOCKET 环境变量 */
    const char* socket_path = getenv("NOTIFY_SOCKET");
    if (!socket_path) {
        return;
    }
    
    /* 创建 UNIX domain socket */
    notifier->sockfd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (notifier->sockfd < 0) {
        return;
    }
    
    notifier->notify_socket = strdup(socket_path);
    notifier->enabled = true;
    
    /* 解析 WATCHDOG_USEC */
    const char* watchdog_str = getenv("WATCHDOG_USEC");
    if (watchdog_str) {
        notifier->watchdog_usec = strtoull(watchdog_str, NULL, 10);
    }
}

void systemd_notifier_destroy(systemd_notifier_t* notifier) {
    if (notifier->sockfd >= 0) {
        close(notifier->sockfd);
        notifier->sockfd = -1;
    }
    
    if (notifier->notify_socket) {
        free(notifier->notify_socket);
        notifier->notify_socket = NULL;
    }
    
    notifier->enabled = false;
}

bool systemd_notifier_is_enabled(const systemd_notifier_t* notifier) {
    return notifier->enabled;
}

bool systemd_notifier_ready(systemd_notifier_t* notifier) {
    return send_notify(notifier, "READY=1");
}

bool systemd_notifier_status(systemd_notifier_t* notifier, const char* status) {
    if (!notifier->enabled || !status) {
        return false;
    }
    
    char message[256];
    snprintf(message, sizeof(message), "STATUS=%s", status);
    return send_notify(notifier, message);
}

bool systemd_notifier_stopping(systemd_notifier_t* notifier) {
    return send_notify(notifier, "STOPPING=1");
}

bool systemd_notifier_watchdog(systemd_notifier_t* notifier) {
    return send_notify(notifier, "WATCHDOG=1");
}

uint64_t systemd_notifier_get_watchdog_usec(const systemd_notifier_t* notifier) {
    return notifier->watchdog_usec;
}
