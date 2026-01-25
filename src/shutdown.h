#ifndef OPENUPS_SHUTDOWN_H
#define OPENUPS_SHUTDOWN_H

#include "config.h"
#include "logger.h"
#include <stdbool.h>

/* 根据配置触发关机；use_systemctl=true 时使用 systemctl。 */
void shutdown_trigger(const config_t* config, logger_t* logger, bool use_systemctl);

#endif /* OPENUPS_SHUTDOWN_H */
