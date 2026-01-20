#ifndef OPENUPS_SHUTDOWN_H
#define OPENUPS_SHUTDOWN_H

#include "config.h"
#include "logger.h"
#include <stdbool.h>

/* 根据配置触发关机（内部使用 fork/execvp，不经过 shell）
 * use_systemctl:
 *   - true: 使用 systemctl poweroff
 *   - false: 使用 /sbin/shutdown
 */
void shutdown_trigger(const config_t* config, logger_t* logger, bool use_systemctl);

#endif /* OPENUPS_SHUTDOWN_H */
