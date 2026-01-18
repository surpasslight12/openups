#ifndef OPENUPS_SYSTEMD_H
#define OPENUPS_SYSTEMD_H

#include <stdbool.h>
#include <stdint.h>

/* systemd 通知器结构 */
typedef struct {
    bool enabled;
    char* notify_socket;
    int sockfd;
    uint64_t watchdog_usec;
} systemd_notifier_t;

/* 初始化 systemd 通知器 */
void systemd_notifier_init(systemd_notifier_t* restrict notifier);

/* 销毁 systemd 通知器 */
void systemd_notifier_destroy(systemd_notifier_t* restrict notifier);

/* 检查 systemd 是否启用 */
[[nodiscard]] bool systemd_notifier_is_enabled(const systemd_notifier_t* restrict notifier);

/* 通知 systemd 就绪 */
[[nodiscard]] bool systemd_notifier_ready(systemd_notifier_t* restrict notifier);

/* 通知 systemd 状态 */
[[nodiscard]] bool systemd_notifier_status(systemd_notifier_t* restrict notifier, 
                                           const char* restrict status);

/* 通知 systemd 停止 */
[[nodiscard]] bool systemd_notifier_stopping(systemd_notifier_t* restrict notifier);

/* 发送 watchdog 心跳 */
[[nodiscard]] bool systemd_notifier_watchdog(systemd_notifier_t* restrict notifier);

#endif /* OPENUPS_SYSTEMD_H */
