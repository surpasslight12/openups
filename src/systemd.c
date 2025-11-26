#include "systemd.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

/* 发送 systemd 通知
 * 实现: 通过 UNIX domain socket (SOCK_DGRAM) 发送消息到 systemd
 * 消息格式: "STATUS=...", "READY=1", "STOPPING=1", "WATCHDOG=1" 等
 */
static bool send_notify(systemd_notifier_t* restrict notifier, const char* restrict message) {
    if (notifier == nullptr || message == nullptr || 
        !notifier->enabled || notifier->notify_socket == nullptr) {
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
    
    /* 重试发送直到成功或遇到非 EINTR 错误 */
    ssize_t sent;
    do {
        sent = sendto(notifier->sockfd, message, strlen(message), 0,
                      (struct sockaddr*)&addr, sizeof(addr));
    } while (sent < 0 && errno == EINTR);
    
    return sent >= 0;
}

void systemd_notifier_init(systemd_notifier_t* restrict notifier) {
    if (notifier == nullptr) {
        return;
    }
    
    notifier->enabled = false;
    notifier->notify_socket = nullptr;
    notifier->sockfd = -1;
    notifier->watchdog_usec = 0;
    
    /* 检查 NOTIFY_SOCKET 环境变量
     * systemd 会为被管理的服务设置此变量
     * 如果不存在，表示程序不是由 systemd 启动的
     */
    const char* socket_path = getenv("NOTIFY_SOCKET");
    if (socket_path == nullptr) {
        return;
    }
    
    /* 创建 UNIX domain socket (SOCK_DGRAM 无连接数据报)
     * 特殊注意: systemd 通知使用数据报方式，不需要预先建立连接
     */
    notifier->sockfd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (notifier->sockfd < 0) {
        return;
    }
    
    notifier->notify_socket = strdup(socket_path);
    if (notifier->notify_socket == nullptr) {
        close(notifier->sockfd);
        notifier->sockfd = -1;
        return;
    }
    notifier->enabled = true;
    
    /* 解析 WATCHDOG_USEC 环境变量
     * systemd 会设置 watchdog 超时时间（微秒）
     * 程序需要每隔 超时时间/2 秒向 systemd 发送一次心跳
     * 如果 0 次未发送心跳，systemd 会强制重启服务
     */
    const char* watchdog_str = getenv("WATCHDOG_USEC");
    if (watchdog_str != nullptr) {
        notifier->watchdog_usec = strtoull(watchdog_str, nullptr, 10);
    }
}

void systemd_notifier_destroy(systemd_notifier_t* restrict notifier) {
    if (notifier == nullptr) {
        return;
    }
    
    if (notifier->sockfd >= 0) {
        close(notifier->sockfd);
        notifier->sockfd = -1;
    }
    
    if (notifier->notify_socket != nullptr) {
        free(notifier->notify_socket);
        notifier->notify_socket = nullptr;
    }
    
    notifier->enabled = false;
}

bool systemd_notifier_is_enabled(const systemd_notifier_t* restrict notifier) {
    return notifier != nullptr && notifier->enabled;
}

bool systemd_notifier_ready(systemd_notifier_t* restrict notifier) {
    return send_notify(notifier, "READY=1");
}

bool systemd_notifier_status(systemd_notifier_t* restrict notifier, const char* restrict status) {
    if (notifier == nullptr || !notifier->enabled || status == nullptr) {
        return false;
    }
    
    /* 向 systemd 报告当前程序状态
     * 格式: "STATUS=..." (e.g., "STATUS=Monitoring 1.1.1.1")
     * systemd 会在 systemctl status 中显示此信息
     */
    char message[256];
    snprintf(message, sizeof(message), "STATUS=%s", status);
    return send_notify(notifier, message);
}

bool systemd_notifier_stopping(systemd_notifier_t* restrict notifier) {
    return send_notify(notifier, "STOPPING=1");
}

bool systemd_notifier_watchdog(systemd_notifier_t* restrict notifier) {
    /* 发送 watchdog 心跳 (WATCHDOG=1)
     * 程序需要不超过 WATCHDOG_USEC/2 秒便送一次心跳
     * 如果 0 次仍旧送 watchdog 心跳，systemd 会生习泻拧程区段
     */
    return send_notify(notifier, "WATCHDOG=1");
}

uint64_t systemd_notifier_get_watchdog_usec(const systemd_notifier_t* restrict notifier) {
    return (notifier != nullptr) ? notifier->watchdog_usec : 0;
}
