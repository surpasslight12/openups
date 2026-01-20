#ifndef OPENUPS_SYSTEMD_H
#define OPENUPS_SYSTEMD_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>

/* systemd 通知器结构 */
typedef struct {
    bool enabled;
    int sockfd;
    uint64_t watchdog_usec;
    struct sockaddr_un addr;
    socklen_t addr_len;

    /* 轻量级降频：避免 STATUS / WATCHDOG 过于频繁 */
    uint64_t last_watchdog_ms;
    uint64_t last_status_ms;
    char last_status[256];
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

/* 获取建议的 watchdog 心跳间隔（毫秒）。
 * 返回值:
 *   - 0: 未启用 watchdog 或 systemd 不可用
 *   - >0: 建议调用 systemd_notifier_watchdog() 的最小间隔
 */
[[nodiscard]] uint64_t systemd_notifier_watchdog_interval_ms(
    const systemd_notifier_t* restrict notifier);

#endif /* OPENUPS_SYSTEMD_H */
