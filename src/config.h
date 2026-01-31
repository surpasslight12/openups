#ifndef CONFIG_H
#define CONFIG_H

/**
 * @file config.h
 * @brief 配置管理：CLI 参数、环境变量、验证
 */

#include "base.h"

#include <stdbool.h>

/* === 关机模式枚举 === */

typedef enum {
    SHUTDOWN_MODE_IMMEDIATE,
    SHUTDOWN_MODE_DELAYED,
    SHUTDOWN_MODE_LOG_ONLY
} shutdown_mode_t;

typedef struct {
    char target[256];
    int interval_sec;
    int fail_threshold;
    int timeout_ms;
    int payload_size;
    int max_retries;
    bool use_ipv6;

    shutdown_mode_t shutdown_mode;
    int delay_minutes;
    bool dry_run;

    bool enable_timestamp;
    log_level_t log_level;

    bool enable_systemd;
    bool enable_watchdog;
} config_t;

void config_init_default(config_t* restrict config);
void config_load_from_env(config_t* restrict config);
[[nodiscard]] bool config_load_from_cmdline(config_t* restrict config, int argc,
                                            char** restrict argv);
[[nodiscard]] bool config_validate(const config_t* restrict config, char* restrict error_msg,
                                   size_t error_size);
void config_print(const config_t* restrict config);
void config_print_usage(void);
void config_print_version(void);

[[nodiscard]] const char* shutdown_mode_to_string(shutdown_mode_t mode);
[[nodiscard]] shutdown_mode_t string_to_shutdown_mode(const char* restrict str);
[[nodiscard]] bool shutdown_mode_parse(const char* restrict str,
                                       shutdown_mode_t* restrict out_mode);

#endif /* CONFIG_H */
