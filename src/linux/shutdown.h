#ifndef SHUTDOWN_H
#define SHUTDOWN_H

/**
 * @file shutdown.h
 * @brief 关机触发：命令构建、fork/exec 执行
 */

#include "config.h"

#include <stdbool.h>

OPENUPS_COLD void shutdown_trigger(const config_t* config, logger_t* logger, bool use_systemctl_poweroff);

#endif /* SHUTDOWN_H */
