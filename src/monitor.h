#ifndef OPENUPS_MONITOR_H
#define OPENUPS_MONITOR_H

#include "openups.h"

bool openups_ctx_init(openups_ctx_t *restrict ctx,
                      const config_t *restrict config,
                      char *restrict error_msg, size_t error_size);
void openups_ctx_destroy(openups_ctx_t *restrict ctx);
int openups_reactor_run(openups_ctx_t *restrict ctx);

#endif // OPENUPS_MONITOR_H